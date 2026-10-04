#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <lucent/analysis/related_shots.hpp>

namespace lucent::analysis {

namespace {

constexpr const char* kWhere = "analysis.related";
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

void append(std::string& text, const std::string& part) {
    text += text.empty() ? part : "; " + part;
}

double aspect(image::Size s) noexcept {
    return s.height > 0 ? static_cast<double>(s.width) / static_cast<double>(s.height) : 0.0;
}

}  // namespace

std::string_view to_string(GroupBasis basis) noexcept {
    switch (basis) {
        case GroupBasis::Single:
            return "single";
        case GroupBasis::CaptureTime:
            return "capture_time";
        case GroupBasis::User:
            return "user";
    }
    return "invalid";
}

ShotComparison compare_shots(const ShotFeatures& a, const ShotFeatures& b, const GroupingOptions& o) {
    ShotComparison c;
    if (a.capture_seconds && b.capture_seconds) {
        c.gap_seconds = static_cast<double>(*b.capture_seconds - *a.capture_seconds);
    }

    // Visual similarity.
    const double ra = aspect(a.size);
    const double rb = aspect(b.size);
    const bool same_orientation = (a.size.width >= a.size.height) == (b.size.width >= b.size.height);
    c.aspect_change = rb > 0.0 ? std::abs(ra / rb - 1.0) : std::numeric_limits<double>::infinity();
    c.comparable = a.descriptors && b.descriptors && same_orientation && c.aspect_change <= o.max_aspect_change;
    if (c.comparable) {
        double structure = 0.0;
        double colour = 0.0;
        for (std::size_t k = 0; k < kShotCells; ++k) {
            structure += std::abs(static_cast<double>(a.structure[k]) - static_cast<double>(b.structure[k]));
            colour += std::hypot(static_cast<double>(a.colour_u[k]) - static_cast<double>(b.colour_u[k]),
                                 static_cast<double>(a.colour_v[k]) - static_cast<double>(b.colour_v[k]));
        }
        c.structure_ev = structure / static_cast<double>(kShotCells);
        c.colour_layout = colour / static_cast<double>(kShotCells);
        double tones = 0.0;
        for (std::size_t k = 0; k < kShotToneBins; ++k) {
            tones += std::abs(static_cast<double>(a.tones[k]) - static_cast<double>(b.tones[k]));
        }
        c.tone_distance = 0.5 * tones;
        c.similar = c.structure_ev <= o.max_structure_ev && c.colour_layout <= o.max_colour_layout &&
                    c.tone_distance <= o.max_tone_distance;
        if (c.structure_ev > o.max_structure_ev) {
            append(c.reason, std::format("layout differs by {:.2f} EV per cell (limit {:.2f})", c.structure_ev,
                                         o.max_structure_ev));
        }
        if (c.colour_layout > o.max_colour_layout) {
            append(c.reason, std::format("colour layout differs by {:.4f} u'v' per cell (limit {:.4f})",
                                         c.colour_layout, o.max_colour_layout));
        }
        if (c.tone_distance > o.max_tone_distance) {
            append(c.reason, std::format("tone distributions differ by {:.2f} (limit {:.2f})", c.tone_distance,
                                         o.max_tone_distance));
        }
    } else if (!a.descriptors || !b.descriptors) {
        append(c.reason, "no visual descriptors");
    } else if (!same_orientation) {
        append(c.reason, "different orientation");
    } else {
        append(c.reason, std::format("aspect ratios differ by {:.0f} %", 100.0 * c.aspect_change));
    }

    // Illumination.
    if (a.scene_ev && b.scene_ev) {
        c.illumination_verified = true;
        c.scene_ev_change = std::abs(*a.scene_ev - *b.scene_ev);
        if (*c.scene_ev_change > o.max_scene_ev_change) {
            c.illumination_change = true;
            append(c.reason, std::format("scene luminance changes by {:.2f} EV (limit {:.2f})", *c.scene_ev_change,
                                         o.max_scene_ev_change));
        }
    } else {
        c.illumination_change = true;
        append(c.reason, "scene luminance unknown (no f-number, exposure time or ISO): a change of light "
                         "cannot be ruled out");
    }
    if (a.white_u && a.white_v && b.white_u && b.white_v) {
        c.white_uv_change = std::hypot(*a.white_u - *b.white_u, *a.white_v - *b.white_v);
        if (*c.white_uv_change > o.max_white_uv_change) {
            c.illumination_change = true;
            append(c.reason, std::format("as-shot white changes by {:.4f} uv (limit {:.4f})", *c.white_uv_change,
                                         o.max_white_uv_change));
        }
    }
    if (a.rendered_colour && b.rendered_colour) {
        c.rendered_uv_change = std::hypot(a.rendered_u - b.rendered_u, a.rendered_v - b.rendered_v);
        if (*c.rendered_uv_change > o.max_rendered_uv_change) {
            c.illumination_change = true;
            append(c.reason, std::format("as-shot colour changes by {:.4f} u'v' (limit {:.4f})",
                                         *c.rendered_uv_change, o.max_rendered_uv_change));
        }
    }
    return c;
}

Grouping group_related_shots(std::span<const GroupInput> items, GroupingMode mode, const GroupingOptions& o) {
    Grouping g;
    const std::size_t n = items.size();
    g.group_of.assign(n, std::nullopt);
    g.decisions.assign(n, GroupDecision{});
    const auto time_of = [&](std::size_t i) {
        return items[i].shot->capture_seconds.value_or(std::numeric_limits<std::int64_t>::max());
    };
    const auto by_time = [&](std::size_t a, std::size_t b) {
        const std::int64_t ta = time_of(a);
        const std::int64_t tb = time_of(b);
        return ta != tb ? ta < tb : a < b;
    };
    const auto add_group = [&](std::string id, GroupBasis basis, std::vector<std::size_t> members) {
        const std::size_t index = g.groups.size();
        for (const std::size_t m : members) {
            g.group_of[m] = index;
            g.decisions[m].grouped = members.size() >= 2;
        }
        g.groups.push_back(RelatedGroup{std::move(id), basis, std::move(members)});
    };

    // Caller overrides.
    std::map<std::string, std::vector<std::size_t>> user;
    std::vector<std::size_t> timed;
    std::vector<std::size_t> untimed;
    for (std::size_t i = 0; i < n; ++i) {
        if (items[i].shot == nullptr) {
            g.decisions[i].reason = "not analysed";
            continue;
        }
        if (items[i].user_group) {
            user[*items[i].user_group].push_back(i);
        } else if (items[i].shot->capture_seconds) {
            timed.push_back(i);
        } else {
            untimed.push_back(i);
        }
    }
    for (auto& [id, members] : user) {
        std::sort(members.begin(), members.end(), by_time);
        for (const std::size_t m : members) {
            g.decisions[m].reason = std::format("caller group '{}'", id);
        }
        add_group("user:" + id, GroupBasis::User, members);
    }

    // Automatic grouping by capture time.
    std::sort(timed.begin(), timed.end(), by_time);
    std::vector<std::vector<std::size_t>> chains;
    if (mode == GroupingMode::None) {
        for (const std::size_t i : timed) {
            g.decisions[i].reason = "grouping off";
            chains.push_back({i});
        }
    } else {
        std::vector<std::size_t> open;
        for (const std::size_t i : timed) {
            GroupDecision& d = g.decisions[i];
            if (open.empty()) {
                d.reason = "first shot of its sequence";
                open.push_back(i);
                continue;
            }
            const std::size_t previous = open.back();
            const ShotFeatures& shot = *items[i].shot;
            ShotComparison c = compare_shots(*items[previous].shot, shot, o);
            const double gap = c.gap_seconds.value_or(std::numeric_limits<double>::infinity());
            const double span = static_cast<double>(time_of(i) - time_of(open.front()));
            std::string why;
            if (gap > o.max_gap_seconds) {
                append(why, std::format("{:.0f} s after the previous shot (limit {:.0f} s)", gap, o.max_gap_seconds));
            }
            if (span > o.max_span_seconds) {
                append(why, std::format("the sequence would span {:.0f} s (limit {:.0f} s)", span, o.max_span_seconds));
            }
            if (!c.similar || c.illumination_change) {
                append(why, c.reason);
            }
            // Complete linkage for the light: no member may differ.
            if (why.empty()) {
                for (const std::size_t m : open) {
                    if (m == previous) {
                        continue;
                    }
                    const ShotComparison other = compare_shots(*items[m].shot, shot, o);
                    if (other.illumination_change) {
                        append(why, std::format("light differs from item {}: {}", m, other.reason));
                        break;
                    }
                }
            }
            d.previous = previous;
            d.comparison = std::move(c);
            if (why.empty()) {
                d.joined = true;
                d.reason = std::format("joined: {:.0f} s after item {}, similar (layout {:.2f} EV, colour {:.4f}, "
                                       "tones {:.2f}), same light",
                                       gap, previous, d.comparison->structure_ev, d.comparison->colour_layout,
                                       d.comparison->tone_distance);
                open.push_back(i);
            } else {
                d.reason = "new sequence: " + why;
                chains.push_back(std::move(open));
                open = {i};
            }
        }
        if (!open.empty()) {
            chains.push_back(std::move(open));
        }
    }
    std::size_t time_groups = 0;
    std::size_t singles = 0;
    for (std::vector<std::size_t>& chain : chains) {
        if (chain.size() >= 2) {
            add_group(std::format("time-{}", ++time_groups), GroupBasis::CaptureTime, std::move(chain));
        } else {
            add_group(std::format("single-{}", ++singles), GroupBasis::Single, std::move(chain));
        }
    }
    for (const std::size_t i : untimed) {
        g.decisions[i].reason = "no capture time";
        add_group(std::format("single-{}", ++singles), GroupBasis::Single, {i});
    }
    return g;
}

bool is_manual(const engine::DevelopSettings& s, bool color) noexcept {
    using engine::DevelopControl;
    const auto locked = [&](DevelopControl c) { return s.locks.contains(c); };
    const bool tone = locked(DevelopControl::Exposure) && locked(DevelopControl::Shadows) &&
                      locked(DevelopControl::Highlights) && locked(DevelopControl::Contrast);
    return tone && (!color || (locked(DevelopControl::WhiteBalance) && locked(DevelopControl::Saturation)));
}

std::vector<std::optional<ConsistencyTarget>> consistency_targets(const Grouping& grouping,
                                                                  std::span<const ConsistencyMember> members,
                                                                  const ConsistencyOptions& options) {
    std::vector<std::optional<ConsistencyTarget>> out(members.size());
    for (const RelatedGroup& group : grouping.groups) {
        std::vector<std::size_t> available;
        for (const std::size_t m : group.members) {
            if (m < members.size() && members[m].available && members[m].appearance.valid) {
                available.push_back(m);
            }
        }
        if (available.size() < 2) {
            continue;
        }
        std::vector<std::size_t> anchors;
        std::vector<std::size_t> contributors;
        for (const std::size_t m : available) {
            if (members[m].anchor) {
                anchors.push_back(m);
            } else if (!members[m].manual) {
                contributors.push_back(m);
            }
        }
        if (!anchors.empty()) {
            contributors = anchors;
        }
        if (contributors.empty()) {
            continue;
        }
        ConsistencyTarget base;
        base.group_id = group.id;
        base.weight = options.weight;
        base.contributors = contributors.size();
        base.anchored = !anchors.empty();
        std::vector<double> lightness;
        for (const std::size_t m : contributors) {
            lightness.push_back(members[m].appearance.lightness);
        }
        base.lightness = median(lightness);
        std::set<std::string> domains;
        for (const std::size_t m : available) {
            domains.insert(members[m].calibration_domain);
        }
        if (domains.size() > 1) {
            std::vector<double> us;
            std::vector<double> vs;
            for (const std::size_t m : contributors) {
                if (members[m].appearance.colour) {
                    us.push_back(members[m].appearance.u);
                    vs.push_back(members[m].appearance.v);
                }
            }
            if (!us.empty()) {
                base.colour = true;
                base.u = median(us);
                base.v = median(vs);
            }
        }
        base.white_balance_confidence = base.anchored ? 1.0 : 0.5;
        for (const std::size_t m : available) {
            const ConsistencyMember& member = members[m];
            if (member.anchor || member.manual) {
                continue;
            }
            ConsistencyTarget t = base;
            const std::string& domain = member.calibration_domain;
            std::size_t in_domain = 0;
            bool anchored_domain = false;
            for (const std::size_t other : available) {
                if (members[other].calibration_domain == domain) {
                    ++in_domain;
                    anchored_domain = anchored_domain || members[other].anchor;
                }
            }
            std::vector<double> reds;
            std::vector<double> blues;
            for (const std::size_t c : contributors) {
                const Appearance& a = members[c].appearance;
                if (members[c].calibration_domain == domain && a.camera_gains) {
                    reds.push_back(a.log_red);
                    blues.push_back(a.log_blue);
                }
            }
            if (!reds.empty() && (in_domain >= 2 || anchored_domain)) {
                t.camera_white_balance = true;
                t.domain = domain;
                t.log_red = median(reds);
                t.log_blue = median(blues);
                t.domain_contributors = reds.size();
                if (!t.anchored && reds.size() >= 3) {
                    t.white_balance_confidence = 0.6;
                }
            }
            out[m] = std::move(t);
        }
    }
    return out;
}

const AutoResult* RelatedItemResult::final_result() const noexcept {
    if (regularised) {
        return &*regularised;
    }
    return independent ? &*independent : nullptr;
}

Expected<RelatedResult> develop_related_files(const raw::DecoderRegistry& registry,
                                              std::span<const RelatedItem> items, const RelatedOptions& options,
                                              const core::CancellationToken& cancel,
                                              const RelatedCallbacks& callbacks) {
    const auto start = Clock::now();
    if (!std::isfinite(options.consistency.weight) || options.consistency.weight < 0.0 ||
        options.consistency.weight > 10.0) {
        return fail(ErrorCode::InvalidArgument, "the consistency weight must lie in [0, 10]", kWhere);
    }
    RelatedResult result;
    result.items.resize(items.size());
    std::vector<std::optional<AutoSession>> sessions(items.size());
    std::uint64_t kept_bytes = 0;

    // Which sessions are worth keeping for pass 2 (memory only: a reopened session gives the same
    // result): items of a caller group of two or more (in either mode), and, with capture-time
    // grouping, items whose probed capture time lies within the largest gap of another item's.
    std::vector<bool> may_group(items.size(), false);
    std::map<std::string, std::size_t> user_counts;
    for (const RelatedItem& item : items) {
        if (item.user_group) {
            ++user_counts[*item.user_group];
        }
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].user_group) {
            may_group[i] = user_counts[*items[i].user_group] >= 2;
        }
    }
    if (options.grouping != GroupingMode::None) {
        std::vector<std::optional<std::int64_t>> times(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].user_group) {
                continue;
            }
            raw::ProbeOptions probe;
            probe.subimage = items[i].subimage;
            probe.describe_subimages = false;
            probe.limits = options.auto_options.develop.decode_limits;
            if (auto selection = registry.select(items[i].path, probe, cancel)) {
                if (const auto& text = selection->probe.metadata.capture.capture_time_local) {
                    times[i] = parse_capture_time(*text);
                }
            } else if (selection.error().code == ErrorCode::Cancelled) {
                return Unexpected(std::move(selection.error()));
            }
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            for (std::size_t j = 0; j < items.size() && times[i] && !may_group[i]; ++j) {
                may_group[i] = j != i && !items[j].user_group && times[j] &&
                               std::abs(static_cast<double>(*times[i] - *times[j])) <=
                                   options.grouping_options.max_gap_seconds;
            }
        }
    }

    // Pass 1: every item's independent analysis.
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto t = Clock::now();
        RelatedItemResult& r = result.items[i];
        r.anchor = items[i].anchor;
        auto session = AutoSession::open_file(registry, items[i].path, items[i].subimage, items[i].current,
                                              options.auto_options, cancel);
        if (!session) {
            if (session.error().code == ErrorCode::Cancelled) {
                return Unexpected(std::move(session.error()));
            }
            r.error = std::move(session.error());
            r.seconds = seconds_since(t);
            continue;
        }
        r.independent = session->independent();
        r.manual = is_manual(items[i].current, r.independent->shot.color);
        r.settings = r.anchor ? items[i].current : r.independent->settings;
        if (r.anchor) {
            auto appearance = session->appearance_of(items[i].current, cancel);
            if (!appearance) {
                if (appearance.error().code == ErrorCode::Cancelled) {
                    return Unexpected(std::move(appearance.error()));
                }
                r.consistency_error = std::move(appearance.error());
            } else {
                r.anchor_appearance = *appearance;
            }
        }
        if (callbacks.independent) {
            callbacks.independent(i, r, *session);
        }
        if (may_group[i] && kept_bytes + session->retained_bytes() <= options.session_budget_bytes) {
            kept_bytes += session->retained_bytes();
            result.peak_session_bytes = std::max(result.peak_session_bytes, kept_bytes);
            sessions[i] = std::move(*session);
        }
        r.seconds = seconds_since(t);
        if (auto cancelled = cancel.check(kWhere); !cancelled) {
            return Unexpected(std::move(cancelled.error()));
        }
    }

    // Grouping and targets.
    std::vector<GroupInput> inputs(items.size());
    std::vector<ConsistencyMember> members(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const RelatedItemResult& r = result.items[i];
        inputs[i].user_group = items[i].user_group;
        if (!r.independent) {
            continue;
        }
        inputs[i].shot = &r.independent->shot;
        ConsistencyMember& m = members[i];
        m.anchor = r.anchor;
        m.manual = r.manual;
        m.calibration_domain = r.independent->shot.calibration_domain;
        if (r.anchor) {
            m.available = r.anchor_appearance.has_value();
            m.appearance = r.anchor_appearance.value_or(Appearance{});
        } else {
            m.available = true;
            m.appearance = r.independent->appearance;
        }
    }
    result.grouping = group_related_shots(inputs, options.grouping, options.grouping_options);
    const auto targets = consistency_targets(result.grouping, members, options.consistency);
    for (std::size_t i = 0; i < items.size(); ++i) {
        result.items[i].target = targets[i];
        if (!targets[i]) {
            sessions[i].reset();  // not needed by pass 2
        }
    }

    // Pass 2: regularise every targeted member, group by group, and revalidate it.
    std::vector<bool> finalised(items.size(), false);
    for (const RelatedGroup& group : result.grouping.groups) {
        for (const std::size_t i : group.members) {
            RelatedItemResult& r = result.items[i];
            if (!r.target) {
                continue;
            }
            const auto t = Clock::now();
            if (!sessions[i]) {
                auto reopened = AutoSession::open_file(registry, items[i].path, items[i].subimage, items[i].current,
                                                       options.auto_options, cancel);
                if (!reopened) {
                    if (reopened.error().code == ErrorCode::Cancelled) {
                        return Unexpected(std::move(reopened.error()));
                    }
                    r.consistency_error = std::move(reopened.error());
                    r.seconds += seconds_since(t);
                    continue;
                }
                sessions[i] = std::move(*reopened);
                r.reopened = true;
                ++result.reopened;
            }
            auto regularised = sessions[i]->regularise(*r.target, cancel);
            if (!regularised) {
                if (regularised.error().code == ErrorCode::Cancelled) {
                    return Unexpected(std::move(regularised.error()));
                }
                r.consistency_error = std::move(regularised.error());
            } else {
                r.regularised = std::move(*regularised);
                r.settings = r.regularised->settings;
            }
            r.seconds += seconds_since(t);
            finalised[i] = true;
            if (callbacks.final) {
                callbacks.final(i, r, &*sessions[i]);
            }
            sessions[i].reset();
        }
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!finalised[i] && callbacks.final) {
            callbacks.final(i, result.items[i], nullptr);
        }
    }
    result.seconds = seconds_since(start);
    return result;
}

}  // namespace lucent::analysis
