#pragma once

// Records and memory of the DNG opcode stages (private; shared by the W2 render path and the
// develop pipeline). See dng_opcode_processing.hpp and ADR 0014.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <lucent/engine/dng_opcode_processing.hpp>
#include <lucent/engine/render.hpp>
#include <lucent/image/cfa.hpp>

namespace lucent::engine::detail {

inline RenderStage opcode_render_stage(raw::DngOpcodeListId list) noexcept {
    switch (list) {
        case raw::DngOpcodeListId::List1:
            return RenderStage::DngOpcodeList1;
        case raw::DngOpcodeListId::List2:
            return RenderStage::DngOpcodeList2;
        case raw::DngOpcodeListId::List3:
            return RenderStage::DngOpcodeList3;
    }
    return RenderStage::DngOpcodeList1;
}

// The stage record of one opcode list run: counts, the applied opcodes and one line per opcode
// ("opcode_<index>": "<outcome>: <name>: <detail>").
inline StageRecord opcode_stage_record(raw::DngOpcodeListId list, const std::vector<DngOpcodeRecord>& records,
                                       const image::ColorEncoding& encoding, image::Size input_size,
                                       image::Size output_size, double seconds) {
    StageRecord s;
    s.stage = opcode_render_stage(list);
    s.algorithm = std::format("dng-opcode-list-{}", static_cast<int>(list));
    s.version = kDngOpcodeProcessingVersion;
    s.input = encoding;
    s.output = encoding;
    s.input_size = input_size;
    s.output_size = output_size;
    std::string applied;
    std::int64_t applied_count = 0;
    std::uint64_t limited = 0;
    for (const DngOpcodeRecord& r : records) {
        if (r.outcome == OpcodeOutcome::Applied) {
            applied += (applied.empty() ? "" : ", ") + r.name;
            ++applied_count;
        }
        limited += r.limited_samples;
    }
    s.parameters = {
        StageParameter{"opcodes", static_cast<std::int64_t>(records.size())},
        StageParameter{"applied_count", applied_count},
        StageParameter{"applied", applied},
    };
    for (const DngOpcodeRecord& r : records) {
        s.parameters.push_back(StageParameter{std::format("opcode_{}", r.index),
                                              std::format("{}: {}: {}", to_string(r.outcome), r.name, r.detail)});
    }
    s.clipped_samples = limited;
    s.seconds = seconds;
    return s;
}

// Camera R, G, B (or monochrome) multipliers that white balance applied to the planes the
// demosaiced image holds, from the per-channel-index multipliers (Green2 stands in for a missing
// green).
inline std::array<double, 3> plane_multipliers(const std::array<double, 4>& channel,
                                               const std::vector<image::CfaColor>& colors) {
    if (colors.size() == 1) {
        return {channel[0] > 0.0 ? channel[0] : 1.0, 1.0, 1.0};
    }
    std::array<double, 3> rgb{0.0, 0.0, 0.0};
    double green2 = 0.0;
    for (std::size_t i = 0; i < colors.size() && i < 4; ++i) {
        switch (colors[i]) {
            case image::CfaColor::Red:
                rgb[0] = rgb[0] == 0.0 ? channel[i] : rgb[0];
                break;
            case image::CfaColor::Green:
                rgb[1] = rgb[1] == 0.0 ? channel[i] : rgb[1];
                break;
            case image::CfaColor::Green2:
                green2 = green2 == 0.0 ? channel[i] : green2;
                break;
            case image::CfaColor::Blue:
                rgb[2] = rgb[2] == 0.0 ? channel[i] : rgb[2];
                break;
            default:
                break;
        }
    }
    if (rgb[1] == 0.0) {
        rgb[1] = green2;
    }
    for (double& m : rgb) {
        m = m > 0.0 ? m : 1.0;
    }
    return rgb;
}

// Extra pixel memory of the opcode stages beyond the buffer they work on: a List 2 TrimBounds
// copies the sensor image and its mask, a List 3 warp or TrimBounds one RGB image.
struct OpcodeMemory {
    std::uint64_t list2_extra = 0;
    std::uint64_t list3_extra = 0;
};

inline OpcodeMemory opcode_memory(const raw::RawMetadata& metadata, bool enabled, std::uint64_t sensor_bytes,
                                  std::uint64_t mask_bytes, std::uint64_t rgb_bytes) {
    OpcodeMemory memory;
    if (!enabled || !metadata.dng) {
        return memory;
    }
    for (const raw::RequiredOperation& op : metadata.required_operations) {
        if (op.kind != raw::RequiredOperationKind::DngOpcode || !op.supported_by_lucent) {
            continue;
        }
        const bool geometric = op.opcode_id == static_cast<std::uint32_t>(raw::DngOpcodeId::TrimBounds) ||
                               op.opcode_id == static_cast<std::uint32_t>(raw::DngOpcodeId::WarpRectilinear) ||
                               op.opcode_id == static_cast<std::uint32_t>(raw::DngOpcodeId::WarpRectilinear2);
        if (!geometric) {
            continue;
        }
        if (op.stage == raw::OperationStage::LinearRaw) {
            memory.list2_extra = sensor_bytes + mask_bytes;
        } else if (op.stage == raw::OperationStage::AfterDemosaic) {
            memory.list3_extra = rgb_bytes;
        }
    }
    return memory;
}

// The interpretation of a render whose opcode processing was switched off: every opcode that
// would have been applied becomes an issue, so such a render is never reported as complete.
inline void add_disabled_opcode_issues(const DngOpcodeReport& report, raw::Interpretation& interpretation) {
    bool changed = false;
    for (const DngOpcodeRecord& r : report.opcodes) {
        if (r.outcome != OpcodeOutcome::Disabled) {
            continue;
        }
        interpretation.issues.push_back(raw::InterpretationIssue{
            r.mandatory ? raw::InterpretationIssueKind::MandatoryOperationUnsupported
                        : raw::InterpretationIssueKind::OptionalOperationUnsupported,
            std::format("OpcodeList{}[{}] {} was not applied: opcode processing is switched off by the render "
                        "settings",
                        static_cast<int>(r.list), r.index, r.name)});
        changed = true;
    }
    if (changed) {
        interpretation.status = raw::derive_interpretation_status(interpretation.issues);
    }
}

}  // namespace lucent::engine::detail
