#pragma once

// Helpers for the DNG opcode tests: synthetic DNG metadata with decoded opcode parameters,
// attached to synthetic decoded data (sensor_test_support.hpp).

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <lucent/raw/decoded_raw.hpp>
#include <lucent/raw/dng_opcodes.hpp>

#include "sensor_test_support.hpp"

namespace lucent::engine::test_support {

struct TestOpcode {
    raw::DngOpcodeListId list = raw::DngOpcodeListId::List3;
    std::uint32_t id = 0;
    raw::DngOpcodeParameters parameters;
    std::uint32_t flags = 0;  // raw::DngOpcode::kFlagOptional, ...
    raw::DngVersion version{{1, 3, 0, 0}};
    std::string parameter_error;
};

// Gives `d` DNG metadata: a raw IFD of the frame's size (CFA or LinearRaw, CFAPlaneColor R, G, B),
// an optional ActiveArea and the opcodes in list order; then the required operations as the
// decoder would report them (raw::required_operation_for, raw::assess_dng_opcode_operations).
inline void attach_dng(raw::DecodedRaw& d, const std::vector<TestOpcode>& opcodes,
                       std::optional<raw::DngRect> active_area = std::nullopt) {
    raw::DngMetadata dng;
    raw::TiffIfdSummary& ifd = dng.raw_ifd;
    ifd.path = "ifd0";
    ifd.width = d.metadata.geometry.raw_width;
    ifd.height = d.metadata.geometry.raw_height;
    ifd.samples_per_pixel = static_cast<std::uint16_t>(d.metadata.representation.channels);
    const bool mosaic = d.metadata.representation.kind == raw::RawRepresentation::Mosaic;
    ifd.photometric = mosaic ? std::uint16_t{32803} : std::uint16_t{34892};
    if (mosaic) {
        ifd.cfa_plane_color = {0, 1, 2};
    }
    ifd.active_area = active_area;
    std::array<std::uint32_t, 3> next{};
    bool previous_optional_wr2 = false;
    raw::DngOpcodeListId previous_list = raw::DngOpcodeListId::List1;
    for (const TestOpcode& spec : opcodes) {
        raw::DngOpcode opcode;
        opcode.list = spec.list;
        opcode.index = next[static_cast<std::size_t>(spec.list) - 1]++;
        opcode.id = spec.id;
        opcode.min_dng_version = spec.version;
        opcode.flags = spec.flags;
        opcode.parameters = spec.parameters;
        opcode.parameter_error = spec.parameter_error;
        const bool legacy_warp = spec.id == 1 || spec.id == 2;
        opcode.follows_optional_warp_rectilinear2 =
            legacy_warp && previous_optional_wr2 && previous_list == spec.list && opcode.index > 0;
        previous_optional_wr2 = spec.id == 14 && opcode.is_optional();
        previous_list = spec.list;
        ifd.opcodes.push_back(std::move(opcode));
    }
    d.metadata.dng = std::move(dng);
    for (const raw::DngOpcode& opcode : d.metadata.dng->raw_ifd.opcodes) {
        d.metadata.required_operations.push_back(raw::required_operation_for(opcode));
    }
    raw::assess_dng_opcode_operations(d.metadata);
}

inline raw::DngOpcodeArea area(std::uint32_t top, std::uint32_t left, std::uint32_t bottom, std::uint32_t right,
                               std::uint32_t plane = 0, std::uint32_t planes = 1, std::uint32_t row_pitch = 1,
                               std::uint32_t col_pitch = 1) {
    raw::DngOpcodeArea a;
    a.rect = raw::DngOpcodeRect{top, left, bottom, right};
    a.plane = plane;
    a.planes = planes;
    a.row_pitch = row_pitch;
    a.col_pitch = col_pitch;
    return a;
}

// WarpRectilinear from coefficient sets {kr0, kr1, kr2, kr3, kt0, kt1}.
inline raw::DngWarpRectilinear warp(const std::vector<std::array<double, 6>>& sets, double cx = 0.5, double cy = 0.5) {
    raw::DngWarpRectilinear w;
    w.center_x = cx;
    w.center_y = cy;
    for (const auto& s : sets) {
        raw::DngWarpPlane plane;
        plane.radial[0] = s[0];
        plane.radial[2] = s[1];
        plane.radial[4] = s[2];
        plane.radial[6] = s[3];
        plane.tangential = {s[4], s[5]};
        w.planes.push_back(plane);
    }
    return w;
}

inline raw::DngGainMap gain_map(const raw::DngOpcodeArea& a, std::uint32_t points_v, std::uint32_t points_h,
                                double spacing_v, double spacing_h, double origin_v, double origin_h,
                                std::uint32_t map_planes, std::vector<float> gains) {
    raw::DngGainMap g;
    g.area = a;
    g.points_v = points_v;
    g.points_h = points_h;
    g.spacing_v = spacing_v;
    g.spacing_h = spacing_h;
    g.origin_v = origin_v;
    g.origin_h = origin_h;
    g.map_planes = map_planes;
    g.gains = std::move(gains);
    return g;
}

inline raw::DngRowColumnValues values(const raw::DngOpcodeArea& a, std::vector<float> v) {
    raw::DngRowColumnValues out;
    out.area = a;
    out.values = std::move(v);
    return out;
}

inline const raw::RequiredOperation* operation(const raw::RawMetadata& m, raw::DngOpcodeListId list,
                                               std::uint32_t index) {
    for (const raw::RequiredOperation& op : m.required_operations) {
        if (op.kind == raw::RequiredOperationKind::DngOpcode && op.stage == raw::operation_stage_of(list) &&
            op.index == index) {
            return &op;
        }
    }
    return nullptr;
}

}  // namespace lucent::engine::test_support
