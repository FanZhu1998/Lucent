#include <lucent/workers/protocol.hpp>

#include <algorithm>
#include <climits>
#include <format>
#include <new>
#include <type_traits>
#include <utility>

#include <windows.h>

#include <lucent/core/text.hpp>
#include <lucent/core/version.hpp>

#include "strict_json.hpp"

namespace lucent::workers {

namespace {

using detail::corrupt;
using detail::Json;
using detail::ObjectReader;

constexpr std::string_view kWhereEncode = "workers.protocol.encode";
constexpr std::string_view kWhereFrame = "workers.protocol.frame";
constexpr std::string_view kWhereVersion = "workers.handshake.version";
constexpr std::string_view kWhereBuild = "workers.handshake.build";

constexpr std::uint32_t kMaxSubimage = 65535;

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message), std::string(kWhereEncode), 0};
}

// ---- Small value codecs -----------------------------------------------------------------------

constexpr char kHex[] = "0123456789abcdef";

std::string hex_bytes(std::span<const std::uint8_t> bytes) {
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        text.push_back(kHex[b >> 4]);
        text.push_back(kHex[b & 0x0F]);
    }
    return text;
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;  // upper case is not canonical and refused
}

bool parse_hex_bytes(std::string_view text, std::span<std::uint8_t> out) noexcept {
    if (text.size() != out.size() * 2) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = hex_value(text[2 * i]);
        const int low = hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::string hex_u64(std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (8 * (7 - i)));
    }
    return hex_bytes(bytes);
}

Expected<std::uint64_t> read_hex_u64(ObjectReader& reader, std::string_view name) {
    auto text = reader.string(name, 16);
    if (!text) {
        return Unexpected(std::move(text.error()));
    }
    std::array<std::uint8_t, 8> bytes{};
    if (!parse_hex_bytes(*text, bytes)) {
        return Unexpected(corrupt(std::format("{}: expected 16 lowercase hex digits", reader.member_path(name))));
    }
    std::uint64_t value = 0;
    for (const std::uint8_t b : bytes) {
        value = (value << 8) | b;
    }
    return value;
}

Expected<core::Digest256> read_digest(ObjectReader& reader, std::string_view name) {
    auto text = reader.string(name, 64);
    if (!text) {
        return Unexpected(std::move(text.error()));
    }
    core::Digest256 digest{};
    if (!parse_hex_bytes(*text, digest)) {
        return Unexpected(corrupt(std::format("{}: expected 64 lowercase hex digits", reader.member_path(name))));
    }
    return digest;
}

Expected<std::optional<core::Digest256>> read_optional_digest(ObjectReader& reader, std::string_view name) {
    auto null = reader.is_null(name);
    if (!null) {
        return Unexpected(std::move(null.error()));
    }
    if (*null) {
        return std::optional<core::Digest256>{};
    }
    auto digest = read_digest(reader, name);
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    return std::optional<core::Digest256>(*digest);
}

Json optional_digest_json(const std::optional<core::Digest256>& digest) {
    return digest ? Json(core::to_hex(*digest)) : Json(nullptr);
}

// Paths: absolute, valid Unicode, within the length limit.
Expected<std::string> path_text(const std::filesystem::path& path, std::string_view what) {
    if (path.empty() || !path.is_absolute()) {
        return Unexpected(invalid(std::format("{} must be an absolute path", what)));
    }
    auto text = core::path_to_utf8(path);
    if (!text) {
        return Unexpected(invalid(std::format("{} is not valid Unicode", what)));
    }
    if (text->size() > kMaxPathBytes || text->find('\0') != std::string::npos) {
        return Unexpected(invalid(std::format("{} is too long or contains NUL", what)));
    }
    return text;
}

Expected<std::filesystem::path> read_path(ObjectReader& reader, std::string_view name, bool needs_file_name) {
    auto text = reader.string(name, kMaxPathBytes, /*allow_empty=*/false);
    if (!text) {
        return Unexpected(std::move(text.error()));
    }
    if (text->find('\0') != std::string::npos) {
        return Unexpected(corrupt(std::format("{}: contains NUL", reader.member_path(name))));
    }
    auto path = core::path_from_utf8(*text);
    if (!path || !path->is_absolute() || (needs_file_name && !path->has_filename())) {
        return Unexpected(corrupt(std::format("{}: expected an absolute path{}", reader.member_path(name),
                                              needs_file_name ? " naming a file" : "")));
    }
    return std::move(*path);
}

// Enumerations travel as their persisted numeric values.
template <class Enum>
Expected<Enum> read_enum(ObjectReader& reader, std::string_view name, bool (*known)(std::uint64_t)) {
    auto value = reader.unsigned_integer(name, 255);
    if (!value) {
        return Unexpected(std::move(value.error()));
    }
    if (!known(*value)) {
        return Unexpected(corrupt(std::format("{}: unknown value {}", reader.member_path(name), *value)));
    }
    return static_cast<Enum>(*value);
}

template <class Enum>
std::uint64_t enum_value(Enum value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::underlying_type_t<Enum>>(value));
}

bool known_format(std::uint64_t v) {
    return exporter::to_string(static_cast<exporter::ExportFormat>(v)) != "unknown";
}
bool known_profile(std::uint64_t v) {
    return exporter::to_string(static_cast<exporter::OutputProfile>(v)) != "unknown";
}
bool known_intent(std::uint64_t v) {
    return exporter::to_string(static_cast<exporter::RenderingIntent>(v)) != "unknown";
}
bool known_compression(std::uint64_t v) {
    return exporter::to_string(static_cast<exporter::TiffCompression>(v)) != "unknown";
}
bool known_chroma(std::uint64_t v) {
    return exporter::to_string(static_cast<exporter::ChromaSubsampling>(v)) != "unknown";
}
bool known_bit_depth(std::uint64_t v) {
    return v == 8 || v == 16;
}
bool known_interpretation(std::uint64_t v) {
    return v >= 1 && v <= 5 && raw::to_string(static_cast<raw::InterpretationStatus>(v)) != "unknown";
}
bool known_stage(std::uint64_t v) {
    return v >= 1 && v <= 5;
}
bool known_class(std::uint64_t v) {
    return v >= 1 && v <= 3;
}
bool known_error_code(std::uint64_t v) {
    return v == static_cast<std::uint64_t>(ErrorCode::Unknown) ||
           (v > 1 && v <= 0xFFFF && to_string(static_cast<ErrorCode>(v)) != "unknown");
}

// ---- Builds, identities, memory ---------------------------------------------------------------

Expected<Json> build_json(const BuildIdentity& build) {
    for (const std::string* field : {&build.version, &build.git_commit, &build.compiler, &build.build_type}) {
        if (field->size() > kMaxShortTextBytes || !core::is_valid_utf8(*field)) {
            return Unexpected(invalid("a build identity field is too long or not UTF-8"));
        }
    }
    return Json{{"version", build.version},
                {"git_commit", build.git_commit},
                {"compiler", build.compiler},
                {"build_type", build.build_type}};
}

Expected<BuildIdentity> read_build(ObjectReader& parent) {
    auto reader = parent.object("build");
    if (!reader) {
        return Unexpected(std::move(reader.error()));
    }
    BuildIdentity build;
    std::pair<const char*, std::string*> fields[] = {{"version", &build.version},
                                                     {"git_commit", &build.git_commit},
                                                     {"compiler", &build.compiler},
                                                     {"build_type", &build.build_type}};
    for (auto& [name, target] : fields) {
        auto text = reader->string(name, kMaxShortTextBytes);
        if (!text) {
            return Unexpected(std::move(text.error()));
        }
        *target = std::move(*text);
    }
    if (auto done = reader->finish(); !done) {
        return Unexpected(std::move(done.error()));
    }
    return build;
}

Json identity_json(const core::FileIdentity& identity) {
    return Json{{"volume_serial", hex_u64(identity.volume_serial)},
                {"file_id", hex_bytes(identity.file_id)},
                {"size", identity.size_bytes},
                {"last_write_time", identity.last_write_time}};
}

Expected<core::FileIdentity> read_identity(ObjectReader& parent, std::string_view name) {
    auto reader = parent.object(name);
    if (!reader) {
        return Unexpected(std::move(reader.error()));
    }
    core::FileIdentity identity;
    auto volume = read_hex_u64(*reader, "volume_serial");
    if (!volume) {
        return Unexpected(std::move(volume.error()));
    }
    identity.volume_serial = *volume;
    auto file_id = reader->string("file_id", 32);
    if (!file_id) {
        return Unexpected(std::move(file_id.error()));
    }
    if (!parse_hex_bytes(*file_id, identity.file_id)) {
        return Unexpected(corrupt(std::format("{}: expected 32 lowercase hex digits", reader->member_path("file_id"))));
    }
    auto size = reader->unsigned_integer("size");
    if (!size) {
        return Unexpected(std::move(size.error()));
    }
    identity.size_bytes = *size;
    auto time = reader->signed_integer("last_write_time");
    if (!time) {
        return Unexpected(std::move(time.error()));
    }
    identity.last_write_time = *time;
    if (auto done = reader->finish(); !done) {
        return Unexpected(std::move(done.error()));
    }
    return identity;
}

Json memory_json(const MemorySnapshot& memory) {
    return Json{{"working_set", memory.working_set},
                {"peak_working_set", memory.peak_working_set},
                {"commit", memory.commit},
                {"peak_commit", memory.peak_commit}};
}

Expected<MemorySnapshot> read_memory(ObjectReader& parent) {
    auto reader = parent.object("memory");
    if (!reader) {
        return Unexpected(std::move(reader.error()));
    }
    MemorySnapshot memory;
    std::pair<const char*, std::uint64_t*> fields[] = {{"working_set", &memory.working_set},
                                                       {"peak_working_set", &memory.peak_working_set},
                                                       {"commit", &memory.commit},
                                                       {"peak_commit", &memory.peak_commit}};
    for (auto& [name, target] : fields) {
        auto value = reader->unsigned_integer(name);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        *target = *value;
    }
    if (auto done = reader->finish(); !done) {
        return Unexpected(std::move(done.error()));
    }
    return memory;
}

Expected<std::string> job_id_text(std::string_view id) {
    if (!is_valid_job_id(id)) {
        return Unexpected(invalid("a job id is 1 to 64 characters of [A-Za-z0-9._:-]"));
    }
    return std::string(id);
}

Expected<std::string> read_job_id(ObjectReader& reader) {
    auto id = reader.string("job_id", kMaxJobIdBytes);
    if (!id) {
        return Unexpected(std::move(id.error()));
    }
    if (!is_valid_job_id(*id)) {
        return Unexpected(corrupt(std::format("{}: not a valid job id", reader.member_path("job_id"))));
    }
    return std::move(*id);
}

// Two paths that name the same file text, compared as Windows compares names (ordinal,
// case-insensitive).
bool same_path_text(const std::filesystem::path& a, const std::filesystem::path& b) {
    const std::wstring x = a.lexically_normal().native();
    const std::wstring y = b.lexically_normal().native();
    if (x.size() > INT_MAX || y.size() > INT_MAX) {
        return false;
    }
    return ::CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()),
                                  TRUE) == CSTR_EQUAL;
}

Expected<Json> rights_json(const std::optional<std::string>& artist, const std::optional<std::string>& copyright) {
    Json rights = Json::object();
    for (const auto& [name, value] : {std::pair{"artist", &artist}, std::pair{"copyright", &copyright}}) {
        if (value->has_value() &&
            ((*value)->size() > exporter::kMaxMetadataTextBytes || !core::is_valid_utf8(**value))) {
            return Unexpected(invalid(std::format("the {} text is too long or not UTF-8", name)));
        }
        rights[name] = value->has_value() ? Json(**value) : Json(nullptr);
    }
    return rights;
}

Status read_rights(ObjectReader& parent, std::optional<std::string>& artist, std::optional<std::string>& copyright) {
    auto reader = parent.object("rights");
    if (!reader) {
        return Unexpected(std::move(reader.error()));
    }
    auto a = reader->optional_string("artist", exporter::kMaxMetadataTextBytes);
    if (!a) {
        return Unexpected(std::move(a.error()));
    }
    auto c = reader->optional_string("copyright", exporter::kMaxMetadataTextBytes);
    if (!c) {
        return Unexpected(std::move(c.error()));
    }
    artist = std::move(*a);
    copyright = std::move(*c);
    return reader->finish();
}

Json text_list_json(const std::vector<std::string>& texts) {
    Json list = Json::array();
    for (const std::string& text : texts) {
        if (list.size() == kMaxWarnings) {
            break;
        }
        list.push_back(truncate_utf8(core::sanitize_utf8(text), kMaxWarningBytes));
    }
    return list;
}

Expected<std::vector<std::string>> read_text_list(ObjectReader& reader, std::string_view name) {
    auto list = reader.array(name, kMaxWarnings);
    if (!list) {
        return Unexpected(std::move(list.error()));
    }
    std::vector<std::string> texts;
    for (const Json& text : **list) {
        if (!text.is_string() || text.get_ref<const std::string&>().size() > kMaxWarningBytes) {
            return Unexpected(corrupt(std::format("{}: expected strings within the limit", reader.member_path(name))));
        }
        texts.push_back(text.get<std::string>());
    }
    return texts;
}

// ---- Messages: encode -------------------------------------------------------------------------

Expected<Json> encode_body(const HostHello& m) {
    if (m.min_version < 1 || m.min_version > m.max_version) {
        return Unexpected(invalid("the host's protocol range is empty"));
    }
    if (m.heartbeat_ms < kMinHeartbeatMs || m.heartbeat_ms > kMaxHeartbeatMs) {
        return Unexpected(invalid("the heartbeat interval is out of range"));
    }
    auto build = build_json(m.build);
    if (!build) {
        return Unexpected(std::move(build.error()));
    }
    return Json{{"protocol", std::string(kProtocolName)}, {"min_version", m.min_version}, {"max_version", m.max_version},
                {"build", std::move(*build)},  {"heartbeat_ms", m.heartbeat_ms}, {"host_pid", m.host_pid}};
}

Expected<Json> encode_body(const WorkerHello& m) {
    if (m.version < 1) {
        return Unexpected(invalid("the negotiated version must be at least 1"));
    }
    if (m.test_hooks.size() > kMaxTestHooks) {
        return Unexpected(invalid("too many test hooks"));
    }
    Json hooks = Json::array();
    for (const std::string& hook : m.test_hooks) {
        if (hook.size() > kMaxShortTextBytes || !core::is_valid_utf8(hook)) {
            return Unexpected(invalid("a test hook name is too long or not UTF-8"));
        }
        hooks.push_back(hook);
    }
    auto build = build_json(m.build);
    if (!build) {
        return Unexpected(std::move(build.error()));
    }
    return Json{{"protocol", std::string(kProtocolName)},     {"version", m.version}, {"build", std::move(*build)},
                {"pid", m.pid},                   {"console_window", m.console_window},
                {"test_hooks", std::move(hooks)}};
}

Expected<Json> encode_body(const JobRequest& m) {
    auto id = job_id_text(m.job_id);
    if (!id) {
        return Unexpected(std::move(id.error()));
    }
    auto source = path_text(m.source.path, "the source path");
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    auto temp = path_text(m.temp_path, "the temporary output path");
    if (!temp) {
        return Unexpected(std::move(temp.error()));
    }
    if (!m.temp_path.has_filename()) {
        return Unexpected(invalid("the temporary output path must name a file"));
    }
    if (same_path_text(m.source.path, m.temp_path)) {
        return Unexpected(invalid("the temporary output path is the source path"));
    }
    if (m.source.subimage > kMaxSubimage) {
        return Unexpected(invalid("the subimage index is out of range"));
    }
    if (m.recipe.document.empty() || m.recipe.document.size() > kMaxRecipeBytes ||
        !core::is_valid_utf8(m.recipe.document)) {
        return Unexpected(invalid("the recipe document is empty, too long or not UTF-8"));
    }
    auto digest = core::sha256(std::as_bytes(std::span(m.recipe.document.data(), m.recipe.document.size())));
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    if (*digest != m.recipe.sha256) {
        return Unexpected(invalid("the recipe's SHA-256 does not match its document"));
    }
    if (auto valid = m.output_settings.validate(); !valid) {
        return Unexpected(invalid(std::format("invalid export settings: {}", valid.error().message)));
    }
    const exporter::ExportSettings& s = m.output_settings;
    Json output_settings{{"format", enum_value(s.format)},
                         {"profile", enum_value(s.profile)},
                         {"intent", enum_value(s.transform.intent)},
                         {"black_point_compensation", s.transform.black_point_compensation},
                         {"jpeg_quality", s.jpeg.quality},
                         {"tiff_compression", enum_value(s.tiff.compression)},
                         {"metadata", Json{{"capture", s.metadata.include_capture},
                                           {"copyright", s.metadata.include_copyright},
                                           {"gps", s.metadata.include_gps},
                                           {"software", s.metadata.include_software}}}};
    auto rights = rights_json(m.artist, m.copyright);
    if (!rights) {
        return Unexpected(std::move(rights.error()));
    }
    Json source_json{{"path", std::move(*source)},
                     {"subimage", m.source.subimage},
                     {"identity", identity_json(m.source.identity)},
                     {"sha256", optional_digest_json(m.source.sha256)}};
    return Json{{"job_id", std::move(*id)},
                {"source", std::move(source_json)},
                {"recipe", Json{{"document", m.recipe.document}, {"sha256", core::to_hex(m.recipe.sha256)}}},
                {"export", std::move(output_settings)},
                {"rights", std::move(*rights)},
                {"temp_path", std::move(*temp)},
                {"memory_budget_bytes", m.memory_budget_bytes},
                {"allow_unsupported", m.allow_unsupported}};
}

Expected<Json> encode_body(const CancelJob& m) {
    auto id = job_id_text(m.job_id);
    if (!id) {
        return Unexpected(std::move(id.error()));
    }
    return Json{{"job_id", std::move(*id)}};
}

Expected<Json> encode_body(const Ping& m) { return Json{{"nonce", m.nonce}}; }

Expected<Json> encode_body(const Shutdown&) { return Json::object(); }

Expected<Json> encode_body(const JobProgress& m) {
    auto id = job_id_text(m.job_id);
    if (!id) {
        return Unexpected(std::move(id.error()));
    }
    if (!known_stage(enum_value(m.stage)) || !(m.fraction >= 0.0 && m.fraction <= 1.0)) {
        return Unexpected(invalid("invalid progress stage or fraction"));
    }
    return Json{{"job_id", std::move(*id)}, {"stage", enum_value(m.stage)}, {"fraction", m.fraction}};
}

Expected<Json> encode_body(const Heartbeat& m) {
    Json id = nullptr;
    if (m.job_id) {
        auto text = job_id_text(*m.job_id);
        if (!text) {
            return Unexpected(std::move(text.error()));
        }
        id = std::move(*text);
    }
    return Json{{"sequence", m.sequence}, {"job_id", std::move(id)}, {"memory", memory_json(m.memory)}};
}

Expected<Json> encode_body(const JobResult& m) {
    auto id = job_id_text(m.job_id);
    if (!id) {
        return Unexpected(std::move(id.error()));
    }
    const JobOutput& o = m.output;
    auto temp = path_text(o.temp_path, "the output path");
    if (!temp) {
        return Unexpected(std::move(temp.error()));
    }
    if (!known_format(enum_value(o.format)) || !known_profile(enum_value(o.profile)) ||
        !known_bit_depth(enum_value(o.bit_depth)) ||
        (o.chroma_subsampling && !known_chroma(enum_value(*o.chroma_subsampling))) ||
        !known_interpretation(enum_value(m.render.interpretation))) {
        return Unexpected(invalid("the result holds an unknown enumerator"));
    }
    if (o.width == 0 || o.height == 0) {
        return Unexpected(invalid("the output has no pixels"));
    }
    std::vector<std::pair<const std::string*, const char*>> texts = {
        {&o.encoder, "encoder"},
        {&m.render.demosaic, "demosaic"},
        {&m.render.camera_color, "camera colour"},
        {&m.render.look, "look"}};
    for (const auto& [text, what] : texts) {
        if (text->size() > kMaxShortTextBytes || !core::is_valid_utf8(*text)) {
            return Unexpected(invalid(std::format("the {} text is too long or not UTF-8", what)));
        }
    }
    Json warnings = text_list_json(m.render.warnings);
    Json output{{"temp_path", std::move(*temp)},
                {"sha256", core::to_hex(o.sha256)},
                {"size", o.size_bytes},
                {"format", enum_value(o.format)},
                {"profile", enum_value(o.profile)},
                {"bit_depth", enum_value(o.bit_depth)},
                {"width", o.width},
                {"height", o.height},
                {"icc_sha256", core::to_hex(o.icc_sha256)},
                {"chroma_subsampling", o.chroma_subsampling ? Json(enum_value(*o.chroma_subsampling)) : Json(nullptr)},
                {"encoder", o.encoder},
                {"metadata_notes", text_list_json(o.metadata_notes)}};
    const OutputVerification& v = m.verification;
    Json verification{{"decoded", v.decoded},
                      {"compared", v.compared},
                      {"rehashed", v.rehashed},
                      {"source_unchanged", v.source_unchanged}};
    const RenderSummaryInfo& r = m.render;
    Json render{{"interpretation", enum_value(r.interpretation)},
                {"warnings", std::move(warnings)},
                {"width", r.width},
                {"height", r.height},
                {"demosaic", r.demosaic},
                {"camera_color", r.camera_color},
                {"pipeline_version", r.pipeline_version},
                {"look", r.look},
                {"look_version", r.look_version},
                {"clipped_raw_samples", r.clipped_raw_samples},
                {"limited_display_pixels", r.limited_display_pixels},
                {"estimated_peak_bytes", r.estimated_peak_bytes}};
    const JobTimings& t = m.timings;
    for (const double seconds : {t.prepare_seconds, t.render_seconds, t.encode_seconds, t.write_seconds,
                                 t.verify_seconds, t.total_seconds}) {
        if (!(seconds >= 0.0 && seconds <= 1e9)) {
            return Unexpected(invalid("a job timing is negative or not finite"));
        }
    }
    Json timings{{"prepare_s", t.prepare_seconds}, {"render_s", t.render_seconds},
                 {"encode_s", t.encode_seconds},   {"write_s", t.write_seconds},
                 {"verify_s", t.verify_seconds},   {"total_s", t.total_seconds}};
    return Json{{"job_id", std::move(*id)},
                {"output", std::move(output)},
                {"verification", std::move(verification)},
                {"source",
                 Json{{"identity", identity_json(m.source_identity)}, {"sha256", optional_digest_json(m.source_sha256)}}},
                {"recipe_sha256", core::to_hex(m.recipe_sha256)},
                {"render", std::move(render)},
                {"timings", std::move(timings)},
                {"memory", memory_json(m.memory)}};
}

Expected<Json> encode_body(const JobError& m) {
    Json id = nullptr;
    if (m.job_id) {
        auto text = job_id_text(*m.job_id);
        if (!text) {
            return Unexpected(std::move(text.error()));
        }
        id = std::move(*text);
    }
    if (!known_error_code(enum_value(m.error.code)) || !known_class(enum_value(m.failure_class))) {
        return Unexpected(invalid("the error holds an unknown code or class"));
    }
    return Json{{"job_id", std::move(id)},
                {"code", enum_value(m.error.code)},
                {"where", truncate_utf8(core::sanitize_utf8(m.error.where), kMaxWhereBytes)},
                {"message", truncate_utf8(core::sanitize_utf8(m.error.message), kMaxErrorMessageBytes)},
                {"native_code", m.error.native_code},
                {"class", enum_value(m.failure_class)}};
}

Expected<Json> encode_body(const Pong& m) { return Json{{"nonce", m.nonce}}; }

// ---- Messages: decode -------------------------------------------------------------------------

Status check_protocol_name(ObjectReader& reader) {
    auto name = reader.string("protocol", kMaxShortTextBytes);
    if (!name) {
        return Unexpected(std::move(name.error()));
    }
    if (*name != kProtocolName) {
        return Unexpected(corrupt(std::format("{}: not the '{}' protocol", reader.member_path("protocol"), kProtocolName)));
    }
    return ok();
}

// Reads every member through the reader's accessors; each `get` stores into the message.
#define LUCENT_READ(target, expression)                     \
    do {                                                     \
        auto lucent_read_value = (expression);               \
        if (!lucent_read_value) {                            \
            return Unexpected(std::move(lucent_read_value.error())); \
        }                                                    \
        target = std::move(*lucent_read_value);              \
    } while (false)

#define LUCENT_CHECK(status_expression)                         \
    do {                                                         \
        if (auto lucent_status = (status_expression); !lucent_status) { \
            return Unexpected(std::move(lucent_status.error())); \
        }                                                        \
    } while (false)

Expected<Message> decode_host_hello(ObjectReader& r) {
    HostHello m;
    LUCENT_CHECK(check_protocol_name(r));
    LUCENT_READ(m.min_version, r.uint32("min_version", 1));
    LUCENT_READ(m.max_version, r.uint32("max_version", 1));
    if (m.min_version > m.max_version) {
        return Unexpected(corrupt("host_hello: min_version is above max_version"));
    }
    LUCENT_READ(m.build, read_build(r));
    LUCENT_READ(m.heartbeat_ms, r.uint32("heartbeat_ms", kMinHeartbeatMs, kMaxHeartbeatMs));
    LUCENT_READ(m.host_pid, r.uint32("host_pid"));
    return m;
}

Expected<Message> decode_worker_hello(ObjectReader& r) {
    WorkerHello m;
    LUCENT_CHECK(check_protocol_name(r));
    LUCENT_READ(m.version, r.uint32("version", 1));
    LUCENT_READ(m.build, read_build(r));
    LUCENT_READ(m.pid, r.uint32("pid"));
    LUCENT_READ(m.console_window, r.boolean("console_window"));
    const Json* hooks = nullptr;
    LUCENT_READ(hooks, r.array("test_hooks", kMaxTestHooks));
    for (const Json& hook : *hooks) {
        if (!hook.is_string() || hook.get_ref<const std::string&>().size() > kMaxShortTextBytes) {
            return Unexpected(corrupt("worker_hello.test_hooks: expected short strings"));
        }
        m.test_hooks.push_back(hook.get<std::string>());
    }
    return m;
}

Expected<exporter::ExportSettings> read_export_settings(ObjectReader& parent) {
    auto reader = parent.object("export");
    if (!reader) {
        return Unexpected(std::move(reader.error()));
    }
    ObjectReader& r = *reader;
    exporter::ExportSettings s;
    LUCENT_READ(s.format, read_enum<exporter::ExportFormat>(r, "format", known_format));
    LUCENT_READ(s.profile, read_enum<exporter::OutputProfile>(r, "profile", known_profile));
    LUCENT_READ(s.transform.intent, read_enum<exporter::RenderingIntent>(r, "intent", known_intent));
    LUCENT_READ(s.transform.black_point_compensation, r.boolean("black_point_compensation"));
    std::uint32_t quality = 0;
    LUCENT_READ(quality, r.uint32("jpeg_quality", 1, 100));
    s.jpeg.quality = static_cast<std::int32_t>(quality);
    LUCENT_READ(s.tiff.compression, read_enum<exporter::TiffCompression>(r, "tiff_compression", known_compression));
    {
        auto policy = r.object("metadata");
        if (!policy) {
            return Unexpected(std::move(policy.error()));
        }
        LUCENT_READ(s.metadata.include_capture, policy->boolean("capture"));
        LUCENT_READ(s.metadata.include_copyright, policy->boolean("copyright"));
        LUCENT_READ(s.metadata.include_gps, policy->boolean("gps"));
        LUCENT_READ(s.metadata.include_software, policy->boolean("software"));
        LUCENT_CHECK(policy->finish());
    }
    LUCENT_CHECK(r.finish());
    if (auto valid = s.validate(); !valid) {
        return Unexpected(corrupt(std::format("job.export: {}", valid.error().message)));
    }
    return s;
}

Expected<Message> decode_job(ObjectReader& r) {
    JobRequest m;
    LUCENT_READ(m.job_id, read_job_id(r));
    {
        auto source = r.object("source");
        if (!source) {
            return Unexpected(std::move(source.error()));
        }
        LUCENT_READ(m.source.path, read_path(*source, "path", /*needs_file_name=*/true));
        LUCENT_READ(m.source.subimage, source->uint32("subimage", 0, kMaxSubimage));
        LUCENT_READ(m.source.identity, read_identity(*source, "identity"));
        LUCENT_READ(m.source.sha256, read_optional_digest(*source, "sha256"));
        LUCENT_CHECK(source->finish());
    }
    {
        auto recipe = r.object("recipe");
        if (!recipe) {
            return Unexpected(std::move(recipe.error()));
        }
        LUCENT_READ(m.recipe.document, recipe->string("document", kMaxRecipeBytes, /*allow_empty=*/false));
        LUCENT_READ(m.recipe.sha256, read_digest(*recipe, "sha256"));
        LUCENT_CHECK(recipe->finish());
        auto digest =
            core::sha256(std::as_bytes(std::span(m.recipe.document.data(), m.recipe.document.size())));
        if (!digest) {
            return Unexpected(std::move(digest.error()));
        }
        if (*digest != m.recipe.sha256) {
            return Unexpected(corrupt("job.recipe: the SHA-256 does not match the document"));
        }
    }
    LUCENT_READ(m.output_settings, read_export_settings(r));
    LUCENT_CHECK(read_rights(r, m.artist, m.copyright));
    LUCENT_READ(m.temp_path, read_path(r, "temp_path", /*needs_file_name=*/true));
    if (same_path_text(m.source.path, m.temp_path)) {
        return Unexpected(corrupt("job.temp_path: the temporary output path is the source path"));
    }
    LUCENT_READ(m.memory_budget_bytes, r.unsigned_integer("memory_budget_bytes"));
    LUCENT_READ(m.allow_unsupported, r.boolean("allow_unsupported"));
    return m;
}

Expected<Message> decode_cancel(ObjectReader& r) {
    CancelJob m;
    LUCENT_READ(m.job_id, read_job_id(r));
    return m;
}

Expected<Message> decode_ping(ObjectReader& r) {
    Ping m;
    LUCENT_READ(m.nonce, r.unsigned_integer("nonce"));
    return m;
}

Expected<Message> decode_pong(ObjectReader& r) {
    Pong m;
    LUCENT_READ(m.nonce, r.unsigned_integer("nonce"));
    return m;
}

Expected<Message> decode_progress(ObjectReader& r) {
    JobProgress m;
    LUCENT_READ(m.job_id, read_job_id(r));
    LUCENT_READ(m.stage, read_enum<JobStage>(r, "stage", known_stage));
    LUCENT_READ(m.fraction, r.number("fraction", 0.0, 1.0));
    return m;
}

Expected<std::optional<std::string>> read_optional_job_id(ObjectReader& r) {
    auto id = r.optional_string("job_id", kMaxJobIdBytes);
    if (!id) {
        return Unexpected(std::move(id.error()));
    }
    if (*id && !is_valid_job_id(**id)) {
        return Unexpected(corrupt(std::format("{}: not a valid job id", r.member_path("job_id"))));
    }
    return std::move(*id);
}

Expected<Message> decode_heartbeat(ObjectReader& r) {
    Heartbeat m;
    LUCENT_READ(m.sequence, r.unsigned_integer("sequence"));
    LUCENT_READ(m.job_id, read_optional_job_id(r));
    LUCENT_READ(m.memory, read_memory(r));
    return m;
}

Expected<Message> decode_result(ObjectReader& r) {
    JobResult m;
    LUCENT_READ(m.job_id, read_job_id(r));
    {
        auto output = r.object("output");
        if (!output) {
            return Unexpected(std::move(output.error()));
        }
        ObjectReader& o = *output;
        LUCENT_READ(m.output.temp_path, read_path(o, "temp_path", /*needs_file_name=*/true));
        LUCENT_READ(m.output.sha256, read_digest(o, "sha256"));
        LUCENT_READ(m.output.size_bytes, o.unsigned_integer("size"));
        LUCENT_READ(m.output.format, read_enum<exporter::ExportFormat>(o, "format", known_format));
        LUCENT_READ(m.output.profile, read_enum<exporter::OutputProfile>(o, "profile", known_profile));
        LUCENT_READ(m.output.bit_depth, read_enum<exporter::BitDepth>(o, "bit_depth", known_bit_depth));
        LUCENT_READ(m.output.width, o.uint32("width", 1));
        LUCENT_READ(m.output.height, o.uint32("height", 1));
        LUCENT_READ(m.output.icc_sha256, read_digest(o, "icc_sha256"));
        bool no_chroma = false;
        LUCENT_READ(no_chroma, o.is_null("chroma_subsampling"));
        if (!no_chroma) {
            LUCENT_READ(m.output.chroma_subsampling,
                        read_enum<exporter::ChromaSubsampling>(o, "chroma_subsampling", known_chroma));
        }
        LUCENT_READ(m.output.encoder, o.string("encoder", kMaxShortTextBytes));
        LUCENT_READ(m.output.metadata_notes, read_text_list(o, "metadata_notes"));
        LUCENT_CHECK(o.finish());
    }
    {
        auto verification = r.object("verification");
        if (!verification) {
            return Unexpected(std::move(verification.error()));
        }
        LUCENT_READ(m.verification.decoded, verification->boolean("decoded"));
        LUCENT_READ(m.verification.compared, verification->boolean("compared"));
        LUCENT_READ(m.verification.rehashed, verification->boolean("rehashed"));
        LUCENT_READ(m.verification.source_unchanged, verification->boolean("source_unchanged"));
        LUCENT_CHECK(verification->finish());
    }
    {
        auto source = r.object("source");
        if (!source) {
            return Unexpected(std::move(source.error()));
        }
        LUCENT_READ(m.source_identity, read_identity(*source, "identity"));
        LUCENT_READ(m.source_sha256, read_optional_digest(*source, "sha256"));
        LUCENT_CHECK(source->finish());
    }
    LUCENT_READ(m.recipe_sha256, read_digest(r, "recipe_sha256"));
    {
        auto render = r.object("render");
        if (!render) {
            return Unexpected(std::move(render.error()));
        }
        ObjectReader& d = *render;
        LUCENT_READ(m.render.interpretation,
                    read_enum<raw::InterpretationStatus>(d, "interpretation", known_interpretation));
        LUCENT_READ(m.render.warnings, read_text_list(d, "warnings"));
        LUCENT_READ(m.render.width, d.uint32("width"));
        LUCENT_READ(m.render.height, d.uint32("height"));
        LUCENT_READ(m.render.demosaic, d.string("demosaic", kMaxShortTextBytes));
        LUCENT_READ(m.render.camera_color, d.string("camera_color", kMaxShortTextBytes));
        LUCENT_READ(m.render.pipeline_version, d.uint32("pipeline_version"));
        LUCENT_READ(m.render.look, d.string("look", kMaxShortTextBytes));
        LUCENT_READ(m.render.look_version, d.uint32("look_version"));
        LUCENT_READ(m.render.clipped_raw_samples, d.unsigned_integer("clipped_raw_samples"));
        LUCENT_READ(m.render.limited_display_pixels, d.unsigned_integer("limited_display_pixels"));
        LUCENT_READ(m.render.estimated_peak_bytes, d.unsigned_integer("estimated_peak_bytes"));
        LUCENT_CHECK(d.finish());
    }
    {
        auto timings = r.object("timings");
        if (!timings) {
            return Unexpected(std::move(timings.error()));
        }
        ObjectReader& t = *timings;
        LUCENT_READ(m.timings.prepare_seconds, t.number("prepare_s", 0.0, 1e9));
        LUCENT_READ(m.timings.render_seconds, t.number("render_s", 0.0, 1e9));
        LUCENT_READ(m.timings.encode_seconds, t.number("encode_s", 0.0, 1e9));
        LUCENT_READ(m.timings.write_seconds, t.number("write_s", 0.0, 1e9));
        LUCENT_READ(m.timings.verify_seconds, t.number("verify_s", 0.0, 1e9));
        LUCENT_READ(m.timings.total_seconds, t.number("total_s", 0.0, 1e9));
        LUCENT_CHECK(t.finish());
    }
    LUCENT_READ(m.memory, read_memory(r));
    return m;
}

Expected<Message> decode_error(ObjectReader& r) {
    JobError m;
    LUCENT_READ(m.job_id, read_optional_job_id(r));
    LUCENT_READ(m.error.code, read_enum<ErrorCode>(r, "code", known_error_code));
    LUCENT_READ(m.error.where, r.string("where", kMaxWhereBytes));
    LUCENT_READ(m.error.message, r.string("message", kMaxErrorMessageBytes));
    LUCENT_READ(m.error.native_code, r.signed_integer("native_code"));
    LUCENT_READ(m.failure_class, read_enum<FailureClass>(r, "class", known_class));
    return m;
}

#undef LUCENT_READ
#undef LUCENT_CHECK

struct TypeEntry {
    std::string_view name;
    Direction direction;
    Expected<Message> (*decode)(ObjectReader&);
};

constexpr TypeEntry kTypes[] = {
    {"host_hello", Direction::HostToWorker, decode_host_hello},
    {"worker_hello", Direction::WorkerToHost, decode_worker_hello},
    {"job", Direction::HostToWorker, decode_job},
    {"cancel", Direction::HostToWorker, decode_cancel},
    {"ping", Direction::HostToWorker, decode_ping},
    {"shutdown", Direction::HostToWorker, [](ObjectReader&) -> Expected<Message> { return Shutdown{}; }},
    {"progress", Direction::WorkerToHost, decode_progress},
    {"heartbeat", Direction::WorkerToHost, decode_heartbeat},
    {"result", Direction::WorkerToHost, decode_result},
    {"error", Direction::WorkerToHost, decode_error},
    {"pong", Direction::WorkerToHost, decode_pong},
};

// Index in kTypes equals the variant index of Message.
static_assert(std::variant_size_v<Message> == std::size(kTypes));

}  // namespace

// ---- Public functions -------------------------------------------------------------------------

BuildIdentity current_build_identity() {
    const core::BuildInfo info = core::build_info();
    return BuildIdentity{std::string(info.version), std::string(info.git_commit), std::string(info.compiler),
                         std::string(info.build_type)};
}

std::string describe(const BuildIdentity& build) {
    return std::format("{} (commit {}, {}, {})", build.version, build.git_commit, build.compiler, build.build_type);
}

Expected<std::uint32_t> negotiate_version(std::uint32_t host_min, std::uint32_t host_max, std::uint32_t worker_min,
                                          std::uint32_t worker_max) {
    if (host_min == 0 || worker_min == 0 || host_min > host_max || worker_min > worker_max) {
        return fail(ErrorCode::InvalidArgument, "a protocol version range is empty", std::string(kWhereVersion));
    }
    const std::uint32_t high = std::min(host_max, worker_max);
    const std::uint32_t low = std::max(host_min, worker_min);
    if (low > high) {
        return fail(ErrorCode::VersionMismatch,
                    std::format("the host speaks protocol versions {}..{}, the worker {}..{}: the components "
                                "come from different Lucent releases",
                                host_min, host_max, worker_min, worker_max),
                    std::string(kWhereVersion));
    }
    return high;
}

Status check_same_build(const BuildIdentity& self, const BuildIdentity& peer, std::string_view peer_role) {
    if (self == peer) {
        return ok();
    }
    return fail(ErrorCode::VersionMismatch,
                std::format("the {} is build {}, this process is build {}: Lucent's components must come from "
                            "the same build",
                            peer_role, describe(peer), describe(self)),
                std::string(kWhereBuild));
}

std::string_view to_string(FailureClass value) noexcept {
    switch (value) {
        case FailureClass::Transient:
            return "transient";
        case FailureClass::Deterministic:
            return "deterministic";
        case FailureClass::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

FailureClass default_failure_class(const Error& error) noexcept {
    switch (error.code) {
        case ErrorCode::Cancelled:
            return FailureClass::Cancelled;
        case ErrorCode::IoError:
        case ErrorCode::Timeout:
        case ErrorCode::OutOfMemory:
        case ErrorCode::Unknown:
            return FailureClass::Transient;
        default:
            return FailureClass::Deterministic;
    }
}

std::string_view to_string(JobStage stage) noexcept {
    switch (stage) {
        case JobStage::Preparing:
            return "preparing";
        case JobStage::Rendering:
            return "rendering";
        case JobStage::Encoding:
            return "encoding";
        case JobStage::Writing:
            return "writing";
        case JobStage::Verifying:
            return "verifying";
    }
    return "unknown";
}

Direction direction_of(const Message& message) noexcept { return kTypes[message.index()].direction; }

std::string_view type_name(const Message& message) noexcept { return kTypes[message.index()].name; }

Expected<std::string> encode_message(const Message& message) {
    try {
        auto body = std::visit([](const auto& m) { return encode_body(m); }, message);
        if (!body) {
            return Unexpected(std::move(body.error()));
        }
        (*body)["type"] = std::string(type_name(message));
        // Strict: a string that is not valid UTF-8 throws instead of being replaced.
        std::string text = body->dump(-1, ' ', /*ensure_ascii=*/false, Json::error_handler_t::strict);
        if (text.size() > kMaxMessageBytes) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("the {} message is {} bytes, above the {}-byte limit", type_name(message),
                                    text.size(), kMaxMessageBytes),
                        std::string(kWhereEncode));
        }
        return text;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot encode a message", std::string(kWhereEncode));
    } catch (const Json::exception& e) {
        return fail(ErrorCode::InvalidArgument, std::format("cannot encode a message: {}", e.what()),
                    std::string(kWhereEncode));
    }
}

Expected<Message> decode_message(std::string_view json, Direction direction) {
    if (json.empty() || json.size() > kMaxMessageBytes) {
        return Unexpected(corrupt("the message is empty or above the size limit"));
    }
    try {
        auto parsed = detail::parse_strict(json);
        if (!parsed) {
            return Unexpected(std::move(parsed.error()));
        }
        auto reader = ObjectReader::make(*parsed, "");
        if (!reader) {
            return Unexpected(corrupt("a message is a JSON object"));
        }
        auto type = reader->string("type", kMaxShortTextBytes);
        if (!type) {
            return Unexpected(std::move(type.error()));
        }
        for (const TypeEntry& entry : kTypes) {
            if (entry.name != *type) {
                continue;
            }
            if (entry.direction != direction) {
                return Unexpected(corrupt(std::format("a '{}' message travels the other way", *type)));
            }
            auto scoped = ObjectReader::make(*parsed, std::string(entry.name));
            if (!scoped) {
                return Unexpected(std::move(scoped.error()));
            }
            // The scoped reader reports member paths as "<type>.<member>"; "type" is read here so
            // finish() accepts it.
            auto again = scoped->string("type", kMaxShortTextBytes);
            if (!again) {
                return Unexpected(std::move(again.error()));
            }
            auto message = entry.decode(*scoped);
            if (!message) {
                return Unexpected(std::move(message.error()));
            }
            if (auto done = scoped->finish(); !done) {
                return Unexpected(std::move(done.error()));
            }
            return message;
        }
        return Unexpected(corrupt(std::format("unknown message type '{}'", core::sanitize_utf8(*type))));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot decode a message", std::string(detail::kWhereDecode));
    } catch (const Json::exception& e) {
        return Unexpected(corrupt(std::format("cannot decode a message: {}", e.what())));
    }
}

Expected<std::string> encode_frame(const Message& message) {
    auto json = encode_message(message);
    if (!json) {
        return Unexpected(std::move(json.error()));
    }
    try {
        const auto length = static_cast<std::uint32_t>(json->size());
        std::string frame(kFrameHeaderBytes, '\0');
        for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
            frame[i] = static_cast<char>((length >> (8 * i)) & 0xFF);
        }
        frame += *json;
        return frame;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot frame a message", std::string(kWhereFrame));
    }
}

Expected<std::uint32_t> decode_frame_length(std::span<const std::byte, kFrameHeaderBytes> header) {
    std::uint32_t length = 0;
    for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
        length |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[i])) << (8 * i);
    }
    if (length == 0 || length > kMaxMessageBytes) {
        return fail(ErrorCode::Corrupt,
                    std::format("a frame announces {} bytes; frames hold 1 to {} bytes", length, kMaxMessageBytes),
                    std::string(kWhereFrame));
    }
    return length;
}

bool is_valid_job_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxJobIdBytes) {
        return false;
    }
    for (const char c : id) {
        const bool ok_char = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                             c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok_char) {
            return false;
        }
    }
    return true;
}

std::string truncate_utf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t end = max_bytes;
    // Step back over continuation bytes (10xxxxxx) so that no sequence is split.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string(text.substr(0, end));
}

}  // namespace lucent::workers
