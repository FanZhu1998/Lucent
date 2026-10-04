// DevelopPreviewService (develop_preview.hpp): edit sessions with a cached stage one, latest-wins
// stage-two renders and the refine rule.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lucent/application/develop_preview.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/core/log.hpp>

namespace lucent::app {

namespace {

constexpr std::string_view kWhere = "app.develop";
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Working memory of a stage-two render of `size` and its display products: the RGB float result,
// the local-tone planes (three floats per pixel), the 8-bit image and the clipping mask.
std::uint64_t stage_two_bytes(image::Size size) {
    return size.empty() ? 0 : static_cast<std::uint64_t>(size.area()) * (12 + 12 + 3 + 1);
}

enum class BuildKind : std::uint8_t { Proxy = 1, Full = 2 };
enum class RenderKind : std::uint8_t { ProxyQuick = 1, ProxyFinal = 2, Full = 3 };

int phase_rank(DevelopPhase phase) {
    return phase == DevelopPhase::Final ? 2 : 1;
}

}  // namespace

struct DevelopPreviewService::State : std::enable_shared_from_this<State> {
    struct Request {
        std::uint64_t generation = 0;
        DevelopView view;
        core::Digest256 sha256{};
        Clock::time_point received{};
    };

    struct Job {
        std::uint64_t id = 0;
        TaskHandle handle;
        std::uint64_t generation = 0;
        engine::WhiteBalanceSetting white_balance;
        BuildKind build = BuildKind::Proxy;
        RenderKind render = RenderKind::ProxyQuick;
    };

    struct Session {
        ItemId item;
        SourceItem source;
        DevelopSink sink;

        std::mutex delivery;  // taken before `mutex`; held across a sink call
        std::mutex mutex;
        std::condition_variable idle;
        bool closed = false;
        std::uint64_t outstanding = 0;  // tasks submitted and not finished
        std::uint64_t next_generation = 1;

        std::optional<Request> latest;
        std::uint64_t quick_done = 0;   // newest generation whose quick render ran
        std::uint64_t final_done = 0;   // newest generation whose final frame is done (or failed)
        std::uint64_t rebuild_for = 0;  // settled generation whose quick correction exceeded the tolerance
        std::uint64_t build_failed_for = 0;  // generation whose stage-one build failed
        std::uint64_t delivered_generation = 0;
        DevelopPhase delivered_phase = DevelopPhase::Quick;
        std::uint64_t rebuilt_generation = 0;  // generation a refine rebuilt stage one for

        std::shared_ptr<const engine::StageOneImage> proxy;
        engine::WhiteBalanceSetting proxy_white_balance;
        AdmissionLease proxy_lease;
        std::shared_ptr<const engine::StageOneImage> full;
        engine::WhiteBalanceSetting full_white_balance;
        AdmissionLease full_lease;

        std::optional<Job> build;
        std::optional<Job> render;
        std::uint64_t next_job = 1;

        DevelopSessionStats stats;
    };

    State(Library& l, TaskExecutor& e, std::shared_ptr<const SourceRenderer> r, DevelopPreviewOptions o)
        : library(l), executor(e), renderer(std::move(r)), options(std::move(o)) {}

    Library& library;
    TaskExecutor& executor;
    std::shared_ptr<const SourceRenderer> renderer;
    DevelopPreviewOptions options;

    mutable std::mutex mutex;  // sessions, shut_down
    bool shut_down = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;

    std::shared_ptr<Session> find(ItemId item) const {
        std::lock_guard lock(mutex);
        const auto found = sessions.find(item.value);
        return found == sessions.end() ? nullptr : found->second;
    }

    // ---- Scheduling (all under the session's mutex) ---------------------------------------------

    bool exceeds_tolerance(const engine::RgbMultipliers& correction) const {
        for (const double c : correction) {
            if (!(std::abs(c - 1.0) <= options.refine_tolerance)) {
                return true;
            }
        }
        return false;
    }

    void start_build(const std::shared_ptr<Session>& s, BuildKind kind, const engine::WhiteBalanceSetting& wb,
                     std::uint64_t generation) {
        const std::uint64_t id = s->next_job++;
        auto submitted = executor.submit(Lane::Interactive, [self = shared_from_this(), s, kind, wb,
                                                             generation, id](TaskContext& context) {
            self->run_build(context, s, kind, wb, generation, id);
        });
        if (!submitted) {
            return;  // shutting down
        }
        Job job;
        job.id = id;
        job.handle = *submitted;
        job.generation = generation;
        job.white_balance = wb;
        job.build = kind;
        s->build = std::move(job);
        ++s->outstanding;
    }

    void start_render(const std::shared_ptr<Session>& s, RenderKind kind, const Request& request) {
        std::shared_ptr<const engine::StageOneImage> source = kind == RenderKind::Full ? s->full : s->proxy;
        const bool rebuilt = s->rebuilt_generation == request.generation;
        const std::uint64_t id = s->next_job++;
        auto submitted = executor.submit(
            Lane::Interactive,
            [self = shared_from_this(), s, kind, request, source = std::move(source), rebuilt,
             id](TaskContext& context) { self->run_render(context, s, kind, request, source, rebuilt, id); });
        if (!submitted) {
            return;
        }
        Job job;
        job.id = id;
        job.handle = *submitted;
        job.generation = request.generation;
        job.render = kind;
        s->render = std::move(job);
        ++s->outstanding;
        if (kind == RenderKind::Full) {
            ++s->stats.full_renders;
        } else {
            ++s->stats.proxy_renders;
        }
    }

    // Starts what the newest request needs next (see the header).
    void pump_locked(const std::shared_ptr<Session>& s) {
        if (s->closed || !s->latest) {
            return;
        }
        const Request& r = *s->latest;
        const engine::WhiteBalanceSetting& wb = r.view.settings.white_balance;
        if (!s->proxy) {
            if (!s->build && s->build_failed_for != r.generation) {
                start_build(s, BuildKind::Proxy, wb, r.generation);
            }
            return;
        }
        if (s->quick_done < r.generation) {
            if (!s->render) {
                start_render(s, RenderKind::ProxyQuick, r);
            }
            return;
        }
        if (!r.view.settled || s->final_done >= r.generation || s->render) {
            return;
        }
        // 100 %: the full-resolution stage one at the settled white balance (its build also derives
        // the proxy at that white balance, so no separate proxy refine is needed).
        if (r.view.quality.kind == PreviewQualityKind::Full) {
            if (!s->full || !(s->full_white_balance == wb)) {
                if (!s->build && s->build_failed_for != r.generation) {
                    start_build(s, BuildKind::Full, wb, r.generation);
                }
                return;
            }
            start_render(s, RenderKind::Full, r);
            return;
        }
        // Refine the proxy when the quick frame needed a white-balance correction.
        if (s->rebuild_for == r.generation && !(s->proxy_white_balance == wb)) {
            if (!s->build && s->build_failed_for != r.generation) {
                start_build(s, BuildKind::Proxy, wb, r.generation);
            }
            return;
        }
        // Fit: the proxy has been rebuilt for this request (otherwise the quick frame was final).
        start_render(s, RenderKind::ProxyFinal, r);
    }

    // ---- Tasks ----------------------------------------------------------------------------------

    void run_build(TaskContext& context, const std::shared_ptr<Session>& s, BuildKind kind,
                   const engine::WhiteBalanceSetting& wb, std::uint64_t generation, std::uint64_t job) {
        const core::CancellationToken& cancel = context.cancel();
        engine::StageOneRequest request;
        request.kind = engine::StageOneKind::Interactive;
        request.white_balance = wb;
        request.proxy_long_edge = kind == BuildKind::Proxy ? options.proxy_long_edge : 0;
        Expected<engine::StageOneImage> one = fail(ErrorCode::Cancelled, "cancelled", std::string(kWhere));
        AdmissionLease lease;
        std::optional<engine::StageOneImage> derived;  // a proxy made from a full build
        AdmissionLease derived_lease;
        SourceItem source;
        {
            std::lock_guard lock(s->mutex);
            source = s->source;
        }
        try {
            if (!cancel.is_cancelled()) {
                auto estimate = renderer->estimate(source, request, cancel);
                if (!estimate) {
                    one = Unexpected(std::move(estimate.error()));
                } else if (auto admitted = context.admit(estimate->stage_one_peak); !admitted) {
                    one = Unexpected(std::move(admitted.error()));
                } else {
                    lease = std::move(*admitted);
                    one = renderer->render_stage_one(source, request, cancel);
                }
            }
            if (one) {
                // The kept image stays claimed (retained), the rest of the peak is returned.
                lease.retain_only(one->byte_size());
                if (options.on_metadata) {
                    options.on_metadata(s->item, one->metadata);
                }
                bool refresh_proxy = false;
                if (kind == BuildKind::Full) {
                    std::lock_guard lock(s->mutex);
                    refresh_proxy = !s->proxy || !(s->proxy_white_balance == wb);
                }
                const std::int32_t long_edge = std::max(one->pixels.width(), one->pixels.height());
                if (refresh_proxy && options.proxy_long_edge < long_edge) {
                    // The proxy at the same white balance, without a second decode.
                    const image::Size size = engine::proxy_size(one->pixels.size(), options.proxy_long_edge);
                    const std::uint64_t bytes = static_cast<std::uint64_t>(size.area()) *
                                                static_cast<std::uint64_t>(one->pixels.channels()) *
                                                sizeof(float);
                    if (auto retained = context.admission().retain(bytes, cancel)) {
                        engine::DevelopOptions develop;
                        develop.parallel = options.parallel;
                        if (auto proxy = engine::make_proxy(*one, options.proxy_long_edge, develop, cancel)) {
                            derived = std::move(*proxy);
                            derived_lease = std::move(*retained);
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            one = fail(ErrorCode::OutOfMemory, "stage one ran out of memory", std::string(kWhere));
        } catch (const std::exception& e) {
            one =
                fail(ErrorCode::Internal, std::format("stage one raised: {}", e.what()), std::string(kWhere));
        }

        std::optional<DevelopEvent> event;
        std::unique_lock delivering(s->delivery);
        {
            std::lock_guard lock(s->mutex);
            if (s->build && s->build->id == job) {
                s->build.reset();
            }
            if (!s->closed) {
                if (one) {
                    ++s->stats.stage_one_builds;
                    const bool refine = kind == BuildKind::Proxy ? static_cast<bool>(s->proxy) : true;
                    if (kind == BuildKind::Proxy) {
                        s->proxy = std::make_shared<const engine::StageOneImage>(std::move(*one));
                        s->proxy_white_balance = wb;
                        s->proxy_lease = std::move(lease);
                    } else {
                        s->full = std::make_shared<const engine::StageOneImage>(std::move(*one));
                        s->full_white_balance = wb;
                        s->full_lease = std::move(lease);
                        if (derived) {
                            s->proxy = std::make_shared<const engine::StageOneImage>(std::move(*derived));
                            s->proxy_white_balance = wb;
                            s->proxy_lease = std::move(derived_lease);
                        }
                    }
                    if (refine) {
                        s->rebuilt_generation = generation;
                    }
                } else if (one.error().code != ErrorCode::Cancelled) {
                    s->build_failed_for = generation;
                    if (s->proxy) {
                        s->final_done = std::max(s->final_done, generation);  // the quick frame stays
                    }
                    if (s->latest && s->latest->generation == generation) {
                        event = DevelopEvent{DevelopEvent::Kind::Failed, s->item, generation, nullptr,
                                             one.error()};
                    }
                }
                pump_locked(s);
            }
        }
        if (event) {
            call_sink(*s, *event);
        }
        delivering.unlock();
        finished(*s);
    }

    void run_render(TaskContext& context, const std::shared_ptr<Session>& s, RenderKind kind,
                    const Request& request, const std::shared_ptr<const engine::StageOneImage>& source,
                    bool rebuilt, std::uint64_t job) {
        const core::CancellationToken& cancel = context.cancel();
        Expected<std::shared_ptr<DevelopFrame>> frame =
            fail(ErrorCode::Cancelled, "cancelled", std::string(kWhere));
        try {
            if (!cancel.is_cancelled() && source) {
                const std::uint64_t bytes = stage_two_bytes(source->pixels.size());
                Expected<AdmissionLease> lease = kind == RenderKind::Full
                                                     ? context.admit(bytes)
                                                     : context.admission().retain(bytes, cancel);
                if (!lease) {
                    frame = Unexpected(std::move(lease.error()));
                } else {
                    auto rendered = renderer->render_stage_two(*source, request.view.settings, cancel);
                    if (!rendered) {
                        frame = Unexpected(std::move(rendered.error()));
                    } else {
                        const auto display_start = Clock::now();
                        auto encoded = encode_display_srgb8(rendered->display, cancel, options.parallel);
                        if (!encoded) {
                            frame = Unexpected(std::move(encoded.error()));
                        } else {
                            rendered->display = {};
                            auto out = std::make_shared<DevelopFrame>();
                            out->item = s->item;
                            out->generation = request.generation;
                            out->revision = request.view.revision;
                            out->transient_generation = request.view.transient_generation;
                            out->settings_sha256 = request.sha256;
                            out->full_resolution = kind == RenderKind::Full;
                            out->stage_one_rebuilt = rebuilt;
                            out->image = std::move(*encoded);
                            auto analysis = analyze_display(out->image, options.parallel);
                            if (!analysis) {
                                frame = Unexpected(std::move(analysis.error()));
                            } else {
                                out->histogram = analysis->histogram;
                                out->clipping = std::move(analysis->clipping);
                                out->summary =
                                    summarize_render(rendered->report, source->metadata.interpretation);
                                out->summary.estimated_peak_bytes = bytes;
                                out->stage_two_seconds = rendered->report.stage_two_seconds;
                                out->display_seconds = seconds_since(display_start);
                                out->report = std::make_shared<const engine::DevelopReport>(
                                    std::move(rendered->report));
                                frame = std::move(out);
                            }
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            frame = fail(ErrorCode::OutOfMemory, "stage two ran out of memory", std::string(kWhere));
        } catch (const std::exception& e) {
            frame =
                fail(ErrorCode::Internal, std::format("stage two raised: {}", e.what()), std::string(kWhere));
        }

        std::optional<DevelopEvent> event;
        std::unique_lock delivering(s->delivery);
        {
            std::lock_guard lock(s->mutex);
            if (s->render && s->render->id == job) {
                s->render.reset();
            }
            if (!s->closed) {
                const std::uint64_t g = request.generation;
                if (frame) {
                    DevelopFrame& f = **frame;
                    if (kind == RenderKind::ProxyQuick) {
                        s->quick_done = std::max(s->quick_done, g);
                        const bool exceeds = exceeds_tolerance(f.report->correction);
                        if (request.view.settled && exceeds) {
                            s->rebuild_for = g;
                        }
                        const bool final = request.view.settled && !exceeds &&
                                           request.view.quality.kind == PreviewQualityKind::Fit;
                        f.phase = final ? DevelopPhase::Final : DevelopPhase::Quick;
                    } else {
                        f.phase = DevelopPhase::Final;
                    }
                    if (f.phase == DevelopPhase::Final) {
                        s->final_done = std::max(s->final_done, g);
                    }
                    f.latency_seconds = seconds_since(request.received);
                    const bool newer =
                        g > s->delivered_generation || (g == s->delivered_generation &&
                                                        phase_rank(f.phase) > phase_rank(s->delivered_phase));
                    if (newer) {
                        s->delivered_generation = g;
                        s->delivered_phase = f.phase;
                        ++s->stats.frames_delivered;
                        event = DevelopEvent{DevelopEvent::Kind::Frame, s->item, g, std::move(*frame),
                                             std::nullopt};
                    } else {
                        ++s->stats.frames_dropped;
                    }
                } else if (frame.error().code != ErrorCode::Cancelled) {
                    s->quick_done = std::max(s->quick_done, g);
                    s->final_done = std::max(s->final_done, g);
                    if (s->latest && s->latest->generation == g) {
                        event = DevelopEvent{DevelopEvent::Kind::Failed, s->item, g, nullptr, frame.error()};
                    }
                }
                pump_locked(s);
            }
        }
        if (event) {
            call_sink(*s, *event);
        }
        delivering.unlock();
        finished(*s);
    }

    static void call_sink(Session& s, const DevelopEvent& event) {
        try {
            s.sink(event);
        } catch (const std::exception& e) {
            core::log_error(kWhere, "a develop sink threw: {}", e.what());
        } catch (...) {
            core::log_error(kWhere, "a develop sink threw an unknown exception");
        }
    }

    static void finished(Session& s) {
        {
            std::lock_guard lock(s.mutex);
            --s.outstanding;
        }
        s.idle.notify_all();
    }

    // Cancels the session's work and drops its caches (no waiting). Requires s.mutex.
    static void stop_locked(Session& s) {
        s.closed = true;
        if (s.build) {
            s.build->handle.cancel();
        }
        if (s.render) {
            s.render->handle.cancel();
        }
        s.proxy.reset();
        s.proxy_lease.release();
        s.full.reset();
        s.full_lease.release();
    }

    // Drops the caches of a session whose file changed (the next request rebuilds them).
    static void invalidate_locked(Session& s) {
        if (s.build) {
            s.build->handle.cancel();
        }
        if (s.render) {
            s.render->handle.cancel();
        }
        s.proxy.reset();
        s.proxy_lease.release();
        s.full.reset();
        s.full_lease.release();
        s.build_failed_for = 0;
    }

    static void wait_idle(Session& s) {
        std::unique_lock lock(s.mutex);
        s.idle.wait(lock, [&] { return s.outstanding == 0; });
    }
};

DevelopPreviewService::DevelopPreviewService(Library& library, TaskExecutor& executor,
                                             std::shared_ptr<const SourceRenderer> renderer,
                                             DevelopPreviewOptions options)
    : state_(std::make_shared<State>(library, executor, std::move(renderer), std::move(options))) {
    subscription_ = library.subscribe([weak = std::weak_ptr<State>(state_)](const LibraryChange& change) {
        if (change.kind != LibraryChangeKind::Removed && change.kind != LibraryChangeKind::Refreshed) {
            return;
        }
        auto state = weak.lock();
        if (!state) {
            return;
        }
        for (const ItemId id : change.items) {
            std::shared_ptr<State::Session> s = state->find(id);
            if (!s) {
                continue;
            }
            std::optional<SourceItem> fresh = state->library.find(id);
            std::lock_guard lock(s->mutex);
            if (change.kind == LibraryChangeKind::Removed || !fresh) {
                // Requests now fail; close() still has to be called (it waits for the tasks).
                State::stop_locked(*s);
            } else {
                s->source = std::move(*fresh);
                State::invalidate_locked(*s);
            }
        }
    });
}

DevelopPreviewService::~DevelopPreviewService() {
    subscription_.reset();
    std::vector<std::shared_ptr<State::Session>> sessions;
    {
        std::lock_guard lock(state_->mutex);
        state_->shut_down = true;
        for (auto& [id, s] : state_->sessions) {
            sessions.push_back(s);
        }
        state_->sessions.clear();
    }
    for (const auto& s : sessions) {
        {
            std::lock_guard lock(s->mutex);
            State::stop_locked(*s);
        }
        State::wait_idle(*s);
    }
}

Status DevelopPreviewService::open(ItemId item, DevelopSink sink) {
    if (!sink) {
        return fail(ErrorCode::InvalidArgument, "the develop sink is empty", std::string(kWhere));
    }
    if (!state_->renderer) {
        return fail(ErrorCode::InvalidArgument, "the develop service has no renderer", std::string(kWhere));
    }
    if (state_->options.proxy_long_edge < 64 ||
        state_->options.proxy_long_edge > PreviewQuality::kMaxLongEdge ||
        !(state_->options.refine_tolerance >= 0.0)) {
        return fail(ErrorCode::InvalidArgument, "invalid develop preview options", std::string(kWhere));
    }
    std::optional<SourceItem> source = state_->library.find(item);
    if (!source) {
        return fail(ErrorCode::NotFound, std::format("no library item {}", item.value), std::string(kWhere));
    }
    try {
        auto session = std::make_shared<State::Session>();
        session->item = item;
        session->source = std::move(*source);
        session->sink = std::move(sink);
        session->stats.item = item;
        std::lock_guard lock(state_->mutex);
        if (state_->shut_down) {
            return fail(ErrorCode::Cancelled, "the develop service is shutting down", std::string(kWhere));
        }
        if (state_->sessions.contains(item.value)) {
            return fail(ErrorCode::AlreadyExists, std::format("item {} is already open", item.value),
                        std::string(kWhere));
        }
        state_->sessions.emplace(item.value, std::move(session));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot open the session", std::string(kWhere));
    }
    return ok();
}

Expected<std::uint64_t> DevelopPreviewService::request(ItemId item, const DevelopView& view) {
    if (auto valid = view.quality.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    auto document = write_recipe(view.settings);  // validates
    if (!document) {
        return Unexpected(std::move(document.error()));
    }
    std::shared_ptr<State::Session> s = state_->find(item);
    if (!s) {
        return fail(ErrorCode::NotFound, std::format("item {} is not open", item.value), std::string(kWhere));
    }
    State::Request request;
    try {
        request.view = view;  // the only allocation: before anything changes
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot queue the request", std::string(kWhere));
    }
    request.sha256 = document->sha256;
    std::lock_guard lock(s->mutex);
    if (s->closed) {
        return fail(ErrorCode::Cancelled, std::format("the session of item {} is closed", item.value),
                    std::string(kWhere));
    }
    request.generation = s->next_generation++;
    request.received = Clock::now();
    if (s->latest && s->quick_done < s->latest->generation &&
        !(s->render && s->render->generation == s->latest->generation)) {
        ++s->stats.requests_coalesced;  // replaced before it was rendered
    }
    ++s->stats.requests;
    const engine::WhiteBalanceSetting& wb = view.settings.white_balance;
    // A rebuild that the new request makes useless is cancelled (the first proxy is always needed).
    if (s->build && s->proxy) {
        const bool same_white = s->build->white_balance == wb;
        const bool wanted =
            view.settled && same_white &&
            (s->build->build == BuildKind::Proxy || view.quality.kind == PreviewQualityKind::Full);
        const bool keep = wanted || (!view.settled && same_white);
        if (!keep) {
            s->build->handle.cancel();
            s->build.reset();
        }
    }
    if (s->render && s->render->render == RenderKind::Full) {
        s->render->handle.cancel();
        s->render.reset();
    }
    // A full-resolution stage one at another white balance is stale once a settled request moves on.
    if (view.settled && s->full && !(s->full_white_balance == wb)) {
        s->full.reset();
        s->full_lease.release();
    }
    s->latest = std::move(request);
    state_->pump_locked(s);
    return s->latest->generation;
}

void DevelopPreviewService::release_full_resolution(ItemId item) {
    std::shared_ptr<State::Session> s = state_->find(item);
    if (!s) {
        return;
    }
    std::lock_guard lock(s->mutex);
    if (s->build && s->build->build == BuildKind::Full) {
        s->build->handle.cancel();
        s->build.reset();
    }
    s->full.reset();
    s->full_lease.release();
}

void DevelopPreviewService::close(ItemId item) {
    std::shared_ptr<State::Session> s;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->sessions.find(item.value);
        if (found == state_->sessions.end()) {
            return;
        }
        s = std::move(found->second);
        state_->sessions.erase(found);
    }
    {
        std::lock_guard lock(s->mutex);
        State::stop_locked(*s);
    }
    State::wait_idle(*s);
}

std::optional<DevelopSessionStats> DevelopPreviewService::stats(ItemId item) const {
    std::shared_ptr<State::Session> s = state_->find(item);
    if (!s) {
        return std::nullopt;
    }
    std::lock_guard lock(s->mutex);
    DevelopSessionStats stats = s->stats;
    if (s->proxy) {
        stats.proxy_size = s->proxy->pixels.size();
        stats.proxy_bytes = s->proxy->byte_size();
    }
    if (s->full) {
        stats.full_size = s->full->pixels.size();
        stats.full_bytes = s->full->byte_size();
    }
    stats.retained_bytes = s->proxy_lease.bytes() + s->full_lease.bytes();
    return stats;
}

std::vector<ItemId> DevelopPreviewService::open_items() const {
    std::lock_guard lock(state_->mutex);
    std::vector<ItemId> items;
    for (const auto& [id, s] : state_->sessions) {
        items.push_back(ItemId{id});
    }
    std::sort(items.begin(), items.end());
    return items;
}

}  // namespace lucent::app
