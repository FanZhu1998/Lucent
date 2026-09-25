#include <format>
#include <string>

#include <lucent/raw/dng_opcodes.hpp>

namespace lucent::raw {

namespace {

constexpr std::string_view kWhere = "raw.dng.opcode_list";
constexpr std::size_t kCountBytes = 4;
constexpr std::size_t kOpcodeHeaderBytes = 16;  // ID, version, flags, parameter byte count

// Opcode lists are big-endian regardless of the file byte order (DNG 1.7.1.0, chapter 7).
std::uint32_t load_be32(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[at]) << 24) |
           (std::to_integer<std::uint32_t>(bytes[at + 1]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[at + 2]) << 8) |
           std::to_integer<std::uint32_t>(bytes[at + 3]);
}

}  // namespace

std::string_view dng_opcode_name(std::uint32_t id) noexcept {
    if (!is_known_dng_opcode(id)) {
        return "Unknown";
    }
    switch (static_cast<DngOpcodeId>(id)) {
        case DngOpcodeId::WarpRectilinear:
            return "WarpRectilinear";
        case DngOpcodeId::WarpFisheye:
            return "WarpFisheye";
        case DngOpcodeId::FixVignetteRadial:
            return "FixVignetteRadial";
        case DngOpcodeId::FixBadPixelsConstant:
            return "FixBadPixelsConstant";
        case DngOpcodeId::FixBadPixelsList:
            return "FixBadPixelsList";
        case DngOpcodeId::TrimBounds:
            return "TrimBounds";
        case DngOpcodeId::MapTable:
            return "MapTable";
        case DngOpcodeId::MapPolynomial:
            return "MapPolynomial";
        case DngOpcodeId::GainMap:
            return "GainMap";
        case DngOpcodeId::DeltaPerRow:
            return "DeltaPerRow";
        case DngOpcodeId::DeltaPerColumn:
            return "DeltaPerColumn";
        case DngOpcodeId::ScalePerRow:
            return "ScalePerRow";
        case DngOpcodeId::ScalePerColumn:
            return "ScalePerColumn";
        case DngOpcodeId::WarpRectilinear2:
            return "WarpRectilinear2";
    }
    return "Unknown";
}

Expected<std::vector<DngOpcode>> parse_dng_opcode_list(std::span<const std::byte> payload,
                                                       DngOpcodeListId list, std::uint32_t max_opcodes) {
    const auto list_number = static_cast<unsigned>(list);
    if (list_number < 1 || list_number > 3) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("opcode list number {} is not 1, 2 or 3", list_number), std::string(kWhere));
    }
    if (payload.size() < kCountBytes) {
        return fail(ErrorCode::Corrupt,
                    std::format("OpcodeList{}: payload is {} bytes; the opcode count alone needs {}",
                                list_number, payload.size(), kCountBytes),
                    std::string(kWhere));
    }

    const std::uint32_t count = load_be32(payload, 0);
    const std::size_t capacity = (payload.size() - kCountBytes) / kOpcodeHeaderBytes;
    if (count > capacity) {
        return fail(
            ErrorCode::Corrupt,
            std::format("OpcodeList{}: declares {} opcodes but its {}-byte payload can hold at most {}",
                        list_number, count, payload.size(), capacity),
            std::string(kWhere));
    }
    if (count > max_opcodes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("OpcodeList{}: declares {} opcodes; the limit is {}", list_number, count,
                                max_opcodes),
                    std::string(kWhere));
    }

    std::vector<DngOpcode> opcodes;
    opcodes.reserve(count);
    std::size_t position = kCountBytes;
    bool previous_is_optional_warp_rectilinear2 = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (payload.size() - position < kOpcodeHeaderBytes) {
            return fail(ErrorCode::Corrupt,
                        std::format("OpcodeList{}: header of opcode {} at byte {} is truncated", list_number,
                                    index, position),
                        std::string(kWhere));
        }
        DngOpcode opcode;
        opcode.list = list;
        opcode.index = index;
        opcode.id = load_be32(payload, position);
        opcode.min_dng_version = DngVersion::from_packed(load_be32(payload, position + 4));
        opcode.flags = load_be32(payload, position + 8);
        opcode.parameter_bytes = load_be32(payload, position + 12);
        position += kOpcodeHeaderBytes;

        if (opcode.parameter_bytes > payload.size() - position) {
            return fail(
                ErrorCode::Corrupt,
                std::format("OpcodeList{}: opcode {} ({}, id {}) declares {} parameter bytes at byte {} but "
                            "only {} remain",
                            list_number, index, opcode.name(), opcode.id, opcode.parameter_bytes, position,
                            payload.size() - position),
                std::string(kWhere));
        }
        opcode.parameter_offset = position;
        position += opcode.parameter_bytes;

        const bool is_legacy_warp = opcode.id == static_cast<std::uint32_t>(DngOpcodeId::WarpRectilinear) ||
                                    opcode.id == static_cast<std::uint32_t>(DngOpcodeId::WarpFisheye);
        opcode.follows_optional_warp_rectilinear2 = is_legacy_warp && previous_is_optional_warp_rectilinear2;
        previous_is_optional_warp_rectilinear2 =
            opcode.id == static_cast<std::uint32_t>(DngOpcodeId::WarpRectilinear2) && opcode.is_optional();

        opcodes.push_back(opcode);
    }

    if (position != payload.size()) {
        // The tag count defines the list length exactly; trailing data makes it ambiguous.
        return fail(ErrorCode::Corrupt,
                    std::format("OpcodeList{}: {} trailing bytes after the last of {} opcodes", list_number,
                                payload.size() - position, count),
                    std::string(kWhere));
    }
    return opcodes;
}

}  // namespace lucent::raw
