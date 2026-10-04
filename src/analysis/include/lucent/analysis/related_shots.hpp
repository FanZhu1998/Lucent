#pragma once

// Related-shot consistency, part 2: grouping, group targets and the two-pass flow over files
// (blueprint section 6, "Related-shot consistency" and "Analysis and candidate search" step 5:
// "If enabled, regularize related-shot upstream settings and revalidate them"; ADR 0011, phase 2).
//
// Grouping (group_related_shots), conservative and deterministic:
//   * Caller overrides first: items with a user group id form exactly that group, whatever their
//     time, look or light (an id given to one item keeps it alone). They never join automatic
//     groups.
//   * Automatic grouping by capture time (GroupingMode::CaptureTime): the other analysed items
//     with a capture time, in (time, input index) order. An item joins the open group when
//       - its gap to the group's last shot is at most max_gap_seconds (60 s) and the group's span
//         stays within max_span_seconds (15 min),
//       - it is visually similar to the group's last shot (compare_shots: same orientation and
//         aspect within 10 %; mean |cell difference| of the exposure-normalised structure grids
//         <= 0.75 EV; mean cell u'v' layout difference <= 0.010; tone histograms within a total
//         variation of 0.30), and
//       - no illumination change separates it from ANY member (complete linkage, so light cannot
//         creep through a chain): scene luminance within 1 EV (needs f-number, exposure time and
//         ISO on both: without them a change cannot be ruled out and the item starts a new
//         group), as-shot whites within 0.008 in CIE 1960 uv (when both files can express a
//         white), as-shot renders' mid-tone u'v' within 0.010.
//     Otherwise it starts a new group; the decision and its measured reason are recorded.
//     Items without a capture time stay alone. Cameras may mix: grouping compares colour-managed
//     and exposure-normalised features only.
//   * GroupingMode::None: no automatic grouping; every item without a caller group stays alone
//     (caller groups are explicit and still apply).
//   Groups are numbered in order: user groups ("user:<id>", by id), then automatic groups and
//   single items ("time-<n>" for groups of two or more, "single-<n>") in capture order, then items
//   without a capture time.
//
// Targets (consistency_targets), for every group of at least two members:
//   * contributors: the caller-marked anchors when the group has any ("Locked/manual images can
//     serve as anchors only with explicit user selection": their current settings' appearance);
//     otherwise every analysed member that is not manual (every searched control locked); a
//     manual member that is not an anchor neither contributes nor moves;
//   * lightness: the median of the contributors' median lightness;
//   * colour (u'v', colour-managed): when the group's members span more than one calibration
//     domain, the component-wise median of the contributors' mid-tone u'v';
//   * camera white balance: per calibration domain with at least two members in the group (or an
//     anchor), the component-wise median of the domain contributors' log camera multipliers;
//     never across domains ("never average camera gains");
//   * every analysed member that is neither an anchor nor manual receives a target; anchors keep
//     their current settings.
//
// develop_related_files() runs the flow: a probe of every file's capture time; pass 1, every file's
// independent analysis (AutoSession::open_file), keeping the sessions of items that can be grouped
// (a caller group of two or more, or another item's capture time within max_gap_seconds) within a
// memory budget; grouping; targets; pass 2, each targeted member regularised
// (AutoSession::regularise, revalidated) on its kept session or on a session reopened from the
// file (identical by determinism). A file that fails is recorded and the others continue;
// Cancelled stops the flow.
//
// The thresholds are uncalibrated Natural v1 defaults (ADR 0011).
//
// Thread-safety: the functions are reentrant; callbacks run on the calling thread.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/analysis/auto_settings.hpp>
#include <lucent/analysis/consistency.hpp>
#include <lucent/core/cancellation.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/decoder_registry.hpp>

namespace lucent::analysis {

// ---- Comparing two shots -----------------------------------------------------------------------

struct GroupingOptions {
    double max_gap_seconds = 60.0;
    double max_span_seconds = 900.0;
    double max_aspect_change = 0.10;        // relative aspect-ratio difference
    double max_structure_ev = 0.75;         // mean |cell difference| of the structure grids
    double max_colour_layout = 0.010;       // mean cell u'v' layout difference
    double max_tone_distance = 0.30;        // total variation of the tone histograms
    double max_scene_ev_change = 1.0;       // scene luminance
    double max_white_uv_change = 0.008;     // as-shot white, CIE 1960 uv
    double max_rendered_uv_change = 0.010;  // as-shot render, mid-tone u'v'
};

struct ShotComparison {
    std::optional<double> gap_seconds;  // b's capture time minus a's
    bool comparable = false;            // descriptors on both, same orientation and similar aspect
    double aspect_change = 0.0;
    double structure_ev = 0.0;
    double colour_layout = 0.0;
    double tone_distance = 0.0;
    bool similar = false;
    std::optional<double> scene_ev_change;     // |difference|, when both have it
    std::optional<double> white_uv_change;     // when both have it
    std::optional<double> rendered_uv_change;  // when both have it
    bool illumination_verified = false;        // the scene luminance could be compared
    bool illumination_change = false;          // a change, or a brightness change not ruled out
    std::string reason;  // developer-facing English: why not similar or which change; empty if neither
};

[[nodiscard]] ShotComparison compare_shots(const ShotFeatures& a, const ShotFeatures& b,
                                           const GroupingOptions& options = {});

// ---- Grouping ----------------------------------------------------------------------------------

enum class GroupingMode : std::uint8_t {
    None = 1,         // no automatic grouping (caller groups still apply)
    CaptureTime = 2,  // capture time, visual similarity, illumination (see the file comment)
};

// Persisted (append only).
enum class GroupBasis : std::uint8_t {
    Single = 1,       // alone
    CaptureTime = 2,  // automatic
    User = 3,         // caller override
};

[[nodiscard]] std::string_view to_string(GroupBasis basis) noexcept;  // "single", "capture_time", "user"

struct GroupInput {
    const ShotFeatures* shot = nullptr;      // null: not analysed (left out of every group)
    std::optional<std::string> user_group;   // caller override
};

struct GroupDecision {
    bool grouped = false;                  // in a group of two or more
    bool joined = false;                   // joined the group of the shot before it (automatic)
    std::optional<std::size_t> previous;   // the shot it was compared with
    std::optional<ShotComparison> comparison;
    std::string reason;                    // developer-facing English
};

struct RelatedGroup {
    std::string id;
    GroupBasis basis = GroupBasis::Single;
    std::vector<std::size_t> members;  // input indices, in capture order
};

struct Grouping {
    std::vector<RelatedGroup> groups;
    std::vector<std::optional<std::size_t>> group_of;  // per input: index into groups
    std::vector<GroupDecision> decisions;              // per input
};

[[nodiscard]] Grouping group_related_shots(std::span<const GroupInput> items,
                                           GroupingMode mode = GroupingMode::CaptureTime,
                                           const GroupingOptions& options = {});

// ---- Targets -----------------------------------------------------------------------------------

struct ConsistencyOptions {
    double weight = 1.0;  // ConsistencyTarget::weight, [0, 10]
};

struct ConsistencyMember {
    bool available = false;  // analysed
    // The member's independent choice, or for an anchor the appearance of its current settings.
    Appearance appearance;
    std::string calibration_domain;
    bool anchor = false;  // caller-marked
    bool manual = false;  // every searched control locked
};

// One target per input (std::nullopt for anchors, manual members, failed items and items alone).
[[nodiscard]] std::vector<std::optional<ConsistencyTarget>> consistency_targets(
    const Grouping& grouping, std::span<const ConsistencyMember> members, const ConsistencyOptions& options = {});

// True when every control the search sets is locked in `settings` (white balance and saturation
// count only for colour data).
[[nodiscard]] bool is_manual(const engine::DevelopSettings& settings, bool color) noexcept;

// ---- The flow over files -----------------------------------------------------------------------

struct RelatedItem {
    std::filesystem::path path;
    std::optional<std::uint32_t> subimage;
    engine::DevelopSettings current;
    std::optional<std::string> user_group;  // caller override of the grouping
    bool anchor = false;                    // caller-marked anchor (keeps its current settings)
};

struct RelatedOptions {
    AutoOptions auto_options;
    GroupingMode grouping = GroupingMode::CaptureTime;
    GroupingOptions grouping_options;
    ConsistencyOptions consistency;
    // Sessions kept from pass 1 for pass 2 while their retained memory stays within this; the
    // others are reopened from their files (the same result).
    std::uint64_t session_budget_bytes = std::uint64_t{1536} << 20;
};

struct RelatedItemResult {
    std::optional<Error> error;              // pass 1 failed: nothing else is set
    std::optional<AutoResult> independent;   // pass 1
    std::optional<ConsistencyTarget> target;
    std::optional<AutoResult> regularised;   // pass 2
    std::optional<Error> consistency_error;  // pass 2 failed: the independent result stands
    std::optional<Appearance> anchor_appearance;
    engine::DevelopSettings settings;  // final: regularised, independent, or (anchor) current
    bool anchor = false;
    bool manual = false;
    bool reopened = false;  // pass 2 decoded the file again
    double seconds = 0.0;   // wall time of both passes

    // The regularised result, else the independent one (null after an error).
    [[nodiscard]] const AutoResult* final_result() const noexcept;
};

struct RelatedResult {
    Grouping grouping;
    std::vector<RelatedItemResult> items;
    std::uint64_t peak_session_bytes = 0;  // largest memory of the sessions kept at once
    std::size_t reopened = 0;
    double seconds = 0.0;
};

struct RelatedCallbacks {
    // After an item's pass 1 (its session open), in input order.
    std::function<void(std::size_t, const RelatedItemResult&, AutoSession&)> independent;
    // Once an item's final settings are known: with its session after pass 2, otherwise null.
    std::function<void(std::size_t, const RelatedItemResult&, AutoSession*)> final;
};

// Errors: InvalidArgument for invalid options; Cancelled. Per-file failures are in the items.
[[nodiscard]] Expected<RelatedResult> develop_related_files(const raw::DecoderRegistry& registry,
                                                            std::span<const RelatedItem> items,
                                                            const RelatedOptions& options = {},
                                                            const core::CancellationToken& cancel = {},
                                                            const RelatedCallbacks& callbacks = {});

}  // namespace lucent::analysis
