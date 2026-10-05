#include <bit>
#include <cmath>
#include <format>
#include <new>
#include <string>
#include <utility>

#include <lucent/raw/dng_opcodes.hpp>

namespace lucent::raw {

namespace {

constexpr std::string_view kWhere = "raw.dng.opcode_list";
constexpr std::string_view kWhereParameters = "raw.dng.opcode_parameters";
constexpr std::size_t kCountBytes = 4;
constexpr std::size_t kOpcodeHeaderBytes = 16;  // ID, version, flags, parameter byte count
constexpr std::uint32_t kMaxWarpPlanes = 4;      // DNG colour planes (kMaxColorPlanes of the SDK)
constexpr std::uint32_t kMaxPolynomialDegree = 8;
constexpr std::uint32_t kMaxTableSize = 65536;

// Opcode lists are big-endian regardless of the file byte order (DNG 1.7.1.0, chapter 7).
std::uint32_t load_be32(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[at]) << 24) |
           (std::to_integer<std::uint32_t>(bytes[at + 1]) << 16) |
           (std::to_integer<std::uint32_t>(bytes[at + 2]) << 8) |
           std::to_integer<std::uint32_t>(bytes[at + 3]);
}

// Sequential big-endian reader over one parameter area. Every read is bounds-checked; the first
// failure is kept and later reads return zero, so a parser can read a fixed layout and check
// once (failed() / finish()).
class Reader {
public:
    Reader(std::uint32_t id, std::span<const std::byte> bytes) : id_(id), bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }

    std::uint32_t u32() {
        if (!take(4)) {
            return 0;
        }
        return load_be32(bytes_, position_ - 4);
    }

    std::uint16_t u16() {
        if (!take(2)) {
            return 0;
        }
        return static_cast<std::uint16_t>((std::to_integer<std::uint32_t>(bytes_[position_ - 2]) << 8) |
                                          std::to_integer<std::uint32_t>(bytes_[position_ - 1]));
    }

    float f32(std::string_view what) {
        const bool had_error = failed();
        const float value = std::bit_cast<float>(u32());
        if (!had_error && !failed() && !std::isfinite(value)) {
            invalid(std::format("{} is not finite", what));
        }
        return value;
    }

    double f64(std::string_view what) {
        if (!take(8)) {
            return 0.0;
        }
        const std::uint64_t bits =
            (std::uint64_t{load_be32(bytes_, position_ - 8)} << 32) | load_be32(bytes_, position_ - 4);
        const double value = std::bit_cast<double>(bits);
        if (!std::isfinite(value)) {
            invalid(std::format("{} is not finite", what));
        }
        return value;
    }

    // Requires exactly `bytes` more bytes before a variable part is allocated, so that no count
    // read from the file can make the parser allocate more than the payload holds.
    bool expect_remaining(std::uint64_t bytes, std::string_view layout) {
        if (error_.empty() && bytes != remaining()) {
            error_ = std::format("{} need {} more bytes but {} remain", layout, bytes, remaining());
        }
        return error_.empty();
    }

    void invalid(std::string message) {
        if (error_.empty()) {
            error_ = std::move(message);
        }
    }

    [[nodiscard]] bool failed() const noexcept { return !error_.empty(); }

    // Ok when nothing failed and every byte was consumed.
    Status finish() {
        if (error_.empty() && remaining() != 0) {
            error_ = std::format("{} trailing bytes after the parameters", remaining());
        }
        if (!error_.empty()) {
            return fail(ErrorCode::Corrupt,
                        std::format("{} (id {}) parameters ({} bytes): {}", dng_opcode_name(id_), id_, bytes_.size(),
                                    error_),
                        std::string(kWhereParameters));
        }
        return ok();
    }

private:
    bool take(std::size_t n) {
        if (!error_.empty()) {
            return false;
        }
        if (remaining() < n) {
            error_ = std::format("truncated at byte {} (needs {} more)", position_, n);
            return false;
        }
        position_ += n;
        return true;
    }

    std::uint32_t id_;
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
    std::string error_;
};

void check_center(Reader& r, double x, double y) {
    if (!r.failed() && (!(x >= 0.0 && x <= 1.0) || !(y >= 0.0 && y <= 1.0))) {
        r.invalid(std::format("optical centre ({}, {}) is not inside [0, 1] x [0, 1]", x, y));
    }
}

DngOpcodeRect read_rect(Reader& r) {
    DngOpcodeRect rect;
    rect.top = r.u32();
    rect.left = r.u32();
    rect.bottom = r.u32();
    rect.right = r.u32();
    return rect;
}

DngOpcodeArea read_area(Reader& r) {
    DngOpcodeArea area;
    area.rect = read_rect(r);
    area.plane = r.u32();
    area.planes = r.u32();
    area.row_pitch = r.u32();
    area.col_pitch = r.u32();
    if (r.failed()) {
        return area;
    }
    if (area.rect.bottom < area.rect.top || area.rect.right < area.rect.left) {
        r.invalid(std::format("area (top {}, left {}, bottom {}, right {}) is inverted", area.rect.top, area.rect.left,
                              area.rect.bottom, area.rect.right));
    } else if (area.planes == 0) {
        r.invalid("the area has Planes 0");
    } else if (area.row_pitch == 0 || area.col_pitch == 0) {
        r.invalid(std::format("the area has RowPitch {} and ColPitch {}; both must be at least 1", area.row_pitch,
                              area.col_pitch));
    } else if (area.rect.empty() && (area.row_pitch != 1 || area.col_pitch != 1)) {
        // An empty rectangle stands for the whole image; the DNG SDK rejects it with a pitch.
        r.invalid(std::format("an empty area (the whole image) has RowPitch {} and ColPitch {}; both must be 1",
                              area.row_pitch, area.col_pitch));
    }
    return area;
}

Expected<DngOpcodeParameters> finish(Reader& r, DngOpcodeParameters value) {
    if (auto status = r.finish(); !status) {
        return Unexpected(std::move(status.error()));
    }
    return value;
}

Expected<DngOpcodeParameters> parse_warp(std::uint32_t id, std::span<const std::byte> bytes, bool extended) {
    Reader r(id, bytes);
    DngWarpRectilinear warp;
    warp.extended = extended;
    const std::uint32_t n = r.u32();
    if (!r.failed() && (n < 1 || n > kMaxWarpPlanes)) {
        r.invalid(std::format("N = {} coefficient sets; expected 1 to {}", n, kMaxWarpPlanes));
    }
    const std::uint64_t per_set = extended ? 19 * 8 : 6 * 8;
    const std::uint64_t tail = extended ? 2 * 8 + 4 : 2 * 8;
    if (!r.expect_remaining(per_set * n + tail, std::format("{} coefficient sets", n))) {
        return finish(r, std::move(warp));
    }
    warp.planes.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        DngWarpPlane& plane = warp.planes[i];
        if (extended) {
            for (double& k : plane.radial) {
                k = r.f64("a radial coefficient");
            }
            plane.tangential[0] = r.f64("kt0");
            plane.tangential[1] = r.f64("kt1");
            plane.min_valid_radius = r.f64("min_valid_radius");
            plane.max_valid_radius = r.f64("max_valid_radius");
            if (!r.failed() && !(plane.min_valid_radius >= 0.0 && plane.min_valid_radius < plane.max_valid_radius &&
                                 plane.max_valid_radius <= 1.0)) {
                r.invalid(std::format("coefficient set {}: valid radius [{}, {}] violates 0 <= min < max <= 1", i,
                                      plane.min_valid_radius, plane.max_valid_radius));
            }
        } else {
            for (std::size_t p = 0; p < 4; ++p) {
                plane.radial[2 * p] = r.f64("a radial coefficient");
            }
            plane.tangential[0] = r.f64("kt0");
            plane.tangential[1] = r.f64("kt1");
        }
    }
    warp.center_x = r.f64("the optical centre");
    warp.center_y = r.f64("the optical centre");
    if (extended) {
        warp.reciprocal_radial = r.u32() != 0;
    }
    check_center(r, warp.center_x, warp.center_y);
    return finish(r, std::move(warp));
}

Expected<DngOpcodeParameters> parse_vignette(std::uint32_t id, std::span<const std::byte> bytes) {
    Reader r(id, bytes);
    DngFixVignetteRadial v;
    for (double& k : v.k) {
        k = r.f64("a gain coefficient");
    }
    v.center_x = r.f64("the optical centre");
    v.center_y = r.f64("the optical centre");
    check_center(r, v.center_x, v.center_y);
    return finish(r, v);
}

void check_phase(Reader& r, std::uint32_t phase) {
    if (!r.failed() && phase > 3) {
        r.invalid(std::format("BayerPhase {} is not 0, 1, 2 or 3", phase));
    }
}

Expected<DngOpcodeParameters> parse_bad_constant(std::uint32_t id, std::span<const std::byte> bytes) {
    Reader r(id, bytes);
    DngFixBadPixelsConstant c;
    c.constant = r.u32();
    c.bayer_phase = r.u32();
    check_phase(r, c.bayer_phase);
    return finish(r, c);
}

Expected<DngOpcodeParameters> parse_bad_list(std::uint32_t id, std::span<const std::byte> bytes) {
    Reader r(id, bytes);
    DngFixBadPixelsList list;
    list.bayer_phase = r.u32();
    const std::uint32_t points = r.u32();
    const std::uint32_t rects = r.u32();
    check_phase(r, list.bayer_phase);
    if (!r.expect_remaining(std::uint64_t{points} * 8 + std::uint64_t{rects} * 16,
                            std::format("{} points and {} rectangles", points, rects))) {
        return finish(r, std::move(list));
    }
    list.points.resize(points);
    for (DngBadPoint& p : list.points) {
        p.row = r.u32();
        p.column = r.u32();
    }
    list.rects.resize(rects);
    for (std::uint32_t i = 0; i < rects; ++i) {
        list.rects[i] = read_rect(r);
        const DngOpcodeRect& b = list.rects[i];
        if (!r.failed() && b.empty()) {
            r.invalid(std::format("bad rectangle {} (top {}, left {}, bottom {}, right {}) is empty", i, b.top, b.left,
                                  b.bottom, b.right));
        }
    }
    return finish(r, std::move(list));
}

Expected<DngOpcodeParameters> parse_trim(std::uint32_t id, std::span<const std::byte> bytes) {
    Reader r(id, bytes);
    DngTrimBounds trim;
    trim.bounds = read_rect(r);
    if (!r.failed() && trim.bounds.empty()) {
        r.invalid(std::format("bounds (top {}, left {}, bottom {}, right {}) are empty", trim.bounds.top,
                              trim.bounds.left, trim.bounds.bottom, trim.bounds.right));
    }
    return finish(r, trim);
}

Expected<DngOpcodeParameters> parse_table(std::uint32_t id, std::span<const std::byte> bytes) {
    Reader r(id, bytes);
    DngMapTable map;
    map.area = read_area(r);
    const std::uint32_t size = r.u32();
    if (!r.failed() && (size < 1 || size > kMaxTableSize)) {
        r.invalid(std::format("TableSize {} is not 1 to {}", size, kMaxTableSize));
    }
    if (!r.expect_remaining(std::uint64_t{size} * 2, std::format("{} table entries", size))) {
        return finish(r, std::move(map));
    }
    map.table.resize(size);
    for (std::uint16_t& entry : map.table) {
        entry = r.u16();
    }
    return finish(r, std::move(map));
}

Expected<DngOpcodeParameters> parse_polynomial(std::uint32_t id, std::span<const std::byte> bytes) {
    Reader r(id, bytes);
    DngMapPolynomial map;
    map.area = read_area(r);
    const std::uint32_t degree = r.u32();
    if (!r.failed() && degree > kMaxPolynomialDegree) {
        r.invalid(std::format("Degree {} exceeds the maximum {}", degree, kMaxPolynomialDegree));
    }
    const std::uint64_t terms = std::uint64_t{degree} + 1;
    if (!r.expect_remaining(terms * 8, std::format("{} coefficients", terms))) {
        return finish(r, std::move(map));
    }
    map.coefficients.resize(static_cast<std::size_t>(terms));
    for (double& c : map.coefficients) {
        c = r.f64("a coefficient");
    }
    return finish(r, std::move(map));
}

Expected<DngOpcodeParameters> parse_gain_map(std::uint32_t id, std::span<const std::byte> bytes) {
    Reader r(id, bytes);
    DngGainMap map;
    map.area = read_area(r);
    map.points_v = r.u32();
    map.points_h = r.u32();
    map.spacing_v = r.f64("MapSpacingV");
    map.spacing_h = r.f64("MapSpacingH");
    map.origin_v = r.f64("MapOriginV");
    map.origin_h = r.f64("MapOriginH");
    map.map_planes = r.u32();
    if (!r.failed()) {
        if (map.points_v < 1 || map.points_h < 1 || map.map_planes < 1) {
            r.invalid(std::format("MapPointsV {}, MapPointsH {} and MapPlanes {} must each be at least 1",
                                  map.points_v, map.points_h, map.map_planes));
        } else if ((map.points_v > 1 && !(map.spacing_v > 0.0)) || (map.points_h > 1 && !(map.spacing_h > 0.0))) {
            r.invalid(std::format("MapSpacingV {} / MapSpacingH {} must be positive for more than one point",
                                  map.spacing_v, map.spacing_h));
        }
    }
    // Three file-controlled factors: their product can exceed 64 bits (2^22 x 2^21 x 2^21 wraps to
    // 0), so it is checked against the bytes that remain before it is formed.
    const std::optional<std::uint64_t> count = dng_gain_map_count(map.points_v, map.points_h, map.map_planes);
    const std::string layout = std::format("{} x {} x {} gains", map.points_v, map.points_h, map.map_planes);
    if (!r.failed() && (!count || *count > r.remaining() / 4)) {
        r.invalid(std::format("{} need more than the {} bytes that remain", layout, r.remaining()));
    }
    if (!r.expect_remaining(count.value_or(0) * 4, layout)) {
        return finish(r, std::move(map));
    }
    map.gains.resize(static_cast<std::size_t>(*count));
    for (float& g : map.gains) {
        g = r.f32("a gain");
    }
    return finish(r, std::move(map));
}

Expected<DngOpcodeParameters> parse_row_column(std::uint32_t id, std::span<const std::byte> bytes, bool per_row) {
    Reader r(id, bytes);
    DngRowColumnValues values;
    values.area = read_area(r);
    const std::uint32_t count = r.u32();
    if (!r.failed()) {
        const std::uint64_t expected = per_row ? values.area.affected_rows() : values.area.affected_columns();
        if (count != expected) {
            r.invalid(std::format("Count {} does not match the {} affected {} of the area", count, expected,
                                  per_row ? "rows" : "columns"));
        }
    }
    if (!r.expect_remaining(std::uint64_t{count} * 4, std::format("{} values", count))) {
        return finish(r, std::move(values));
    }
    values.values.resize(count);
    for (float& v : values.values) {
        v = r.f32("a value");
    }
    return finish(r, std::move(values));
}

bool is(const DngOpcode& opcode, DngOpcodeId id) noexcept { return opcode.id == static_cast<std::uint32_t>(id); }

}  // namespace

std::uint64_t DngOpcodeArea::affected_rows() const noexcept {
    if (rect.empty() || row_pitch == 0) {
        return 0;
    }
    return (std::uint64_t{rect.bottom} - rect.top + row_pitch - 1) / row_pitch;
}

std::uint64_t DngOpcodeArea::affected_columns() const noexcept {
    if (rect.empty() || col_pitch == 0) {
        return 0;
    }
    return (std::uint64_t{rect.right} - rect.left + col_pitch - 1) / col_pitch;
}

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

std::optional<DngVersion> dng_opcode_defined_version(std::uint32_t id) noexcept {
    if (!is_known_dng_opcode(id)) {
        return std::nullopt;
    }
    if (id == static_cast<std::uint32_t>(DngOpcodeId::WarpRectilinear2)) {
        return DngVersion{{1, 6, 0, 0}};
    }
    return DngVersion{{1, 3, 0, 0}};
}

Expected<DngOpcodeParameters> parse_dng_opcode_parameters(std::uint32_t id, std::span<const std::byte> parameters) {
    if (!decodes_dng_opcode_parameters(id)) {
        return DngOpcodeParameters{};
    }
    try {
        switch (static_cast<DngOpcodeId>(id)) {
            case DngOpcodeId::WarpRectilinear:
                return parse_warp(id, parameters, false);
            case DngOpcodeId::WarpRectilinear2:
                return parse_warp(id, parameters, true);
            case DngOpcodeId::FixVignetteRadial:
                return parse_vignette(id, parameters);
            case DngOpcodeId::FixBadPixelsConstant:
                return parse_bad_constant(id, parameters);
            case DngOpcodeId::FixBadPixelsList:
                return parse_bad_list(id, parameters);
            case DngOpcodeId::TrimBounds:
                return parse_trim(id, parameters);
            case DngOpcodeId::MapTable:
                return parse_table(id, parameters);
            case DngOpcodeId::MapPolynomial:
                return parse_polynomial(id, parameters);
            case DngOpcodeId::GainMap:
                return parse_gain_map(id, parameters);
            case DngOpcodeId::DeltaPerRow:
            case DngOpcodeId::ScalePerRow:
                return parse_row_column(id, parameters, true);
            case DngOpcodeId::DeltaPerColumn:
            case DngOpcodeId::ScalePerColumn:
                return parse_row_column(id, parameters, false);
            case DngOpcodeId::WarpFisheye:
                break;
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory,
                    std::format("cannot allocate the {}-byte parameters of {}", parameters.size(),
                                dng_opcode_name(id)),
                    std::string(kWhereParameters));
    }
    return DngOpcodeParameters{};
}

Expected<std::vector<DngOpcode>> parse_dng_opcode_list(std::span<const std::byte> payload, DngOpcodeListId list,
                                                       std::uint32_t max_opcodes) {
    const auto list_number = static_cast<unsigned>(list);
    if (list_number < 1 || list_number > 3) {
        return fail(ErrorCode::InvalidArgument, std::format("opcode list number {} is not 1, 2 or 3", list_number),
                    std::string(kWhere));
    }
    if (payload.size() < kCountBytes) {
        return fail(ErrorCode::Corrupt,
                    std::format("OpcodeList{}: payload is {} bytes; the opcode count alone needs {}", list_number,
                                payload.size(), kCountBytes),
                    std::string(kWhere));
    }

    const std::uint32_t count = load_be32(payload, 0);
    const std::size_t capacity = (payload.size() - kCountBytes) / kOpcodeHeaderBytes;
    if (count > capacity) {
        return fail(ErrorCode::Corrupt,
                    std::format("OpcodeList{}: declares {} opcodes but its {}-byte payload can hold at most {}",
                                list_number, count, payload.size(), capacity),
                    std::string(kWhere));
    }
    if (count > max_opcodes) {
        return fail(ErrorCode::LimitExceeded,
                    std::format("OpcodeList{}: declares {} opcodes; the limit is {}", list_number, count, max_opcodes),
                    std::string(kWhere));
    }

    std::vector<DngOpcode> opcodes;
    opcodes.reserve(count);
    std::size_t position = kCountBytes;
    bool previous_is_optional_warp_rectilinear2 = false;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (payload.size() - position < kOpcodeHeaderBytes) {
            return fail(ErrorCode::Corrupt,
                        std::format("OpcodeList{}: header of opcode {} at byte {} is truncated", list_number, index,
                                    position),
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
            return fail(ErrorCode::Corrupt,
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

        opcodes.push_back(std::move(opcode));
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

Status decode_dng_opcode_parameters(std::span<DngOpcode> opcodes, std::span<const std::byte> payload) {
    for (DngOpcode& opcode : opcodes) {
        opcode.parameters = DngOpcodeParameters{};
        opcode.parameter_error.clear();
        if (!decodes_dng_opcode_parameters(opcode.id) || !opcode.version_supported()) {
            continue;
        }
        if (opcode.parameter_offset > payload.size() ||
            opcode.parameter_bytes > payload.size() - opcode.parameter_offset) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("OpcodeList{}[{}]: the parameter area lies outside the payload",
                                    static_cast<int>(opcode.list), opcode.index),
                        std::string(kWhereParameters));
        }
        auto parsed = parse_dng_opcode_parameters(
            opcode.id, payload.subspan(static_cast<std::size_t>(opcode.parameter_offset), opcode.parameter_bytes));
        if (parsed) {
            opcode.parameters = std::move(*parsed);
            continue;
        }
        if (parsed.error().code != ErrorCode::Corrupt) {
            return Unexpected(std::move(parsed.error()));
        }
        opcode.parameter_error = parsed.error().describe();
    }
    return ok();
}

DngOpcodeSupport assess_dng_opcode(const DngOpcode& opcode, const DngOpcodeImage& image) {
    const auto no = [](std::string reason) { return DngOpcodeSupport{false, std::move(reason)}; };
    if (!opcode.is_known()) {
        return no("the opcode ID is not defined by DNG 1.7.1.0");
    }
    if (!opcode.version_supported()) {
        return no(std::format("the opcode declares DNG {}, newer than the supported {}",
                              opcode.min_dng_version.to_string(), kDngSpecificationVersion.to_string()));
    }
    if (is(opcode, DngOpcodeId::WarpFisheye)) {
        return no("WarpFisheye is not implemented");
    }
    if (!image.list_unavailable.empty()) {
        return no(image.list_unavailable);
    }
    if (!opcode.parameter_error.empty()) {
        return no(std::format("its parameters are malformed: {}", opcode.parameter_error));
    }
    if (std::holds_alternative<std::monostate>(opcode.parameters)) {
        return no("its parameters were not decoded");
    }
    const bool list1 = image.list == DngOpcodeListId::List1;
    const bool list3 = image.list == DngOpcodeListId::List3;
    if (list1 && !image.integer_samples) {
        return no("OpcodeList1 on floating-point stored samples is not implemented");
    }
    // The radial opcodes measure r in square pixels; the DNG SDK scales the vertical axis by the
    // pixel aspect ratio first, which Lucent does not implement.
    const bool radial = is(opcode, DngOpcodeId::WarpRectilinear) || is(opcode, DngOpcodeId::WarpRectilinear2) ||
                        is(opcode, DngOpcodeId::FixVignetteRadial);
    if (radial && image.pixel_aspect != 1.0) {
        return no(std::format("radial corrections on non-square pixels (pixel aspect {}) are not implemented",
                              image.pixel_aspect));
    }
    switch (static_cast<DngOpcodeId>(opcode.id)) {
        case DngOpcodeId::WarpRectilinear:
        case DngOpcodeId::WarpRectilinear2: {
            if (!list3) {
                return no("warps are implemented after demosaicing (OpcodeList3) only");
            }
            const auto& warp = std::get<DngWarpRectilinear>(opcode.parameters);
            if (warp.planes.size() != 1 && warp.planes.size() != image.planes) {
                return no(std::format("{} coefficient sets for an image of {} planes (DNG requires 1 or one per "
                                      "plane)",
                                      warp.planes.size(), image.planes));
            }
            return {true, {}};
        }
        case DngOpcodeId::FixBadPixelsConstant:
        case DngOpcodeId::FixBadPixelsList: {
            const bool constant = is(opcode, DngOpcodeId::FixBadPixelsConstant);
            if (list3 || (constant && !list1)) {
                return no(constant ? "FixBadPixelsConstant is implemented on stored samples (OpcodeList1) only"
                                   : "bad-pixel lists are implemented before demosaicing (OpcodeList1 and 2) only");
            }
            if (!image.mosaic || !image.bayer_phase || image.planes != 1) {
                return no("bad-pixel repair is defined for a 2 x 2 Bayer CFA image and this image is not one");
            }
            const std::uint32_t phase = constant ? std::get<DngFixBadPixelsConstant>(opcode.parameters).bayer_phase
                                                 : std::get<DngFixBadPixelsList>(opcode.parameters).bayer_phase;
            const auto green_first = [](std::uint32_t p) { return p == 1 || p == 2; };
            if (green_first(phase) != green_first(static_cast<std::uint32_t>(*image.bayer_phase))) {
                return no(std::format("BayerPhase {} places the green sites differently from the image's CFA "
                                      "(phase {})",
                                      phase, static_cast<int>(*image.bayer_phase)));
            }
            return {true, {}};
        }
        case DngOpcodeId::TrimBounds: {
            if (list1) {
                return no("TrimBounds is implemented in OpcodeList2 and OpcodeList3 only");
            }
            const DngOpcodeRect& b = std::get<DngTrimBounds>(opcode.parameters).bounds;
            if (b.bottom > image.height || b.right > image.width) {
                return no(std::format("the TrimBounds rectangle (bottom {}, right {}) exceeds the {} x {} image",
                                      b.bottom, b.right, image.width, image.height));
            }
            return {true, {}};
        }
        case DngOpcodeId::FixVignetteRadial:
            return {true, {}};
        // The invariants below are what the parser guarantees; they are checked again so that an
        // opcode built otherwise (or altered) is never applied with an inconsistent payload.
        case DngOpcodeId::MapTable: {
            const auto& map = std::get<DngMapTable>(opcode.parameters);
            if (map.table.empty() || map.table.size() > kMaxTableSize) {
                return no(std::format("its table has {} entries (1 to {} are defined)", map.table.size(),
                                      kMaxTableSize));
            }
            return {true, {}};
        }
        case DngOpcodeId::MapPolynomial: {
            const auto& map = std::get<DngMapPolynomial>(opcode.parameters);
            if (map.coefficients.empty() || map.coefficients.size() > kMaxPolynomialDegree + 1) {
                return no(std::format("it has {} coefficients (1 to {} are defined)", map.coefficients.size(),
                                      kMaxPolynomialDegree + 1));
            }
            return {true, {}};
        }
        case DngOpcodeId::GainMap: {
            const auto& map = std::get<DngGainMap>(opcode.parameters);
            if (!map.complete()) {
                return no(std::format("its {} gains do not fill its {} x {} x {} grid", map.gains.size(),
                                      map.points_v, map.points_h, map.map_planes));
            }
            return {true, {}};
        }
        case DngOpcodeId::DeltaPerRow:
        case DngOpcodeId::DeltaPerColumn:
        case DngOpcodeId::ScalePerRow:
        case DngOpcodeId::ScalePerColumn: {
            const auto& values = std::get<DngRowColumnValues>(opcode.parameters);
            if (values.area.rect.empty()) {
                // The DNG SDK reads an empty area as the whole image and then reads past its
                // (empty) table: the opcode is undefined.
                return no("its area is empty, which stands for the whole image, but it holds no value for any row "
                          "or column of it");
            }
            const bool per_row = is(opcode, DngOpcodeId::DeltaPerRow) || is(opcode, DngOpcodeId::ScalePerRow);
            const std::uint64_t expected =
                per_row ? values.area.affected_rows() : values.area.affected_columns();
            if (values.values.size() != expected) {
                return no(std::format("it holds {} values for {} affected {}", values.values.size(), expected,
                                      per_row ? "rows" : "columns"));
            }
            return {true, {}};
        }
        case DngOpcodeId::WarpFisheye:
            break;
    }
    return no("not implemented");
}

}  // namespace lucent::raw
