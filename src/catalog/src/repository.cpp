// ReadTransaction and WriteTransaction: the catalog's queries and changes. Every value reaches
// SQLite as a bound parameter; SQL text is assembled only from the fixed fragments below.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "lucent/catalog/catalog.hpp"
#include "lucent/catalog/location.hpp"
#include "lucent/core/hash.hpp"
#include "lucent/core/text.hpp"

#include "catalog_impl.hpp"
#include "codec.hpp"
#include "sqlite.hpp"

namespace lucent::catalog {

using detail::Connection;
using detail::Statement;
using detail::TransactionGuard;

namespace {

// ---------------------------------------------------------------------------------------------
// Row layouts
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kSourceColumns =
    "s.id, s.path, s.path_key, s.volume_serial, s.file_id, s.size, s.mtime, s.sha256, s.import_mode, "
    "s.origin_path, s.storage_kind, s.state, s.first_seen, s.last_seen, s.origin_volume_serial, "
    "s.origin_file_id, s.origin_size, s.origin_mtime";

constexpr std::string_view kItemColumns =
    "i.id, i.source_id, i.subimage_index, i.make, i.model, i.lens, i.capture_time, i.width, i.height, "
    "i.orientation, i.format, i.support_status, i.support_reasons_json, i.decoder_id, i.decoder_version, "
    "i.probed_at, i.pick, i.rating, s.path, s.state, s.storage_kind, s.sha256 IS NOT NULL, s.path_key, "
    "i.current_revision_id, i.iso, i.exposure_time_s, i.f_number, i.focal_length_mm, i.image_count";
constexpr int kItemPathKeyColumn = 22;

// A revision without its settings document (columns 0 ... 11), and with it (column 12).
// octet_length() takes the size from the record header without loading the document.
constexpr std::string_view kRevisionInfoColumns =
    "r.id, r.item_id, r.revision_number, r.parent_revision_id, r.created_at, r.origin, r.schema_name, "
    "r.schema_version, r.settings_sha256, octet_length(r.settings_json), r.engine_version, r.note";
constexpr int kRevisionSettingsColumn = 12;

// Sort key of an unknown capture time (see items_by_capture_time in schema.cpp).
constexpr const char* kUnknownCaptureTime = "~";

constexpr std::string_view kImportColumns =
    "id, started_at, finished_at, outcome, roots_json, options_json, files_seen, added, refreshed, "
    "unchanged, "
    "duplicates, skipped, failed";

std::string sql(std::initializer_list<std::string_view> parts) {
    std::string text;
    for (const std::string_view part : parts) {
        text.append(part);
    }
    return text;
}

// A bound parameter of dynamically assembled queries.
using Parameter = std::variant<std::int64_t, std::string>;

void bind_all(Statement& statement, const std::vector<Parameter>& parameters) {
    int index = 1;
    for (const Parameter& parameter : parameters) {
        if (const auto* number = std::get_if<std::int64_t>(&parameter)) {
            statement.bind_int64(index, *number);
        } else {
            statement.bind_text(index, std::get<std::string>(parameter));
        }
        ++index;
    }
}

std::span<const std::uint8_t> bytes_of(const core::Digest256& digest) noexcept {
    return digest;
}
std::span<const std::uint8_t> bytes_of(const std::array<std::uint8_t, 16>& id) noexcept {
    return id;
}

Status invalid(std::string_view where, std::string message) {
    return fail(ErrorCode::InvalidArgument, std::move(message), std::string(where));
}

Status check_limit(std::uint32_t limit, std::string_view where) {
    if (limit < 1 || limit > kMaxPageSize) {
        return invalid(where, std::format("limit must be 1 ... {}", kMaxPageSize));
    }
    return ok();
}

Status check_text(std::string_view text, std::string_view field, std::string_view where) {
    if (!core::is_valid_utf8(text)) {
        return invalid(where, std::format("{} is not valid UTF-8", field));
    }
    return ok();
}

Expected<std::string> path_text(const std::filesystem::path& path, std::string_view where) {
    Expected<std::string> text = core::path_to_utf8(path);
    if (!text) {
        return fail(ErrorCode::InvalidArgument,
                    "path is not valid UTF-16: '" + core::path_to_utf8_lossy(path) + "'", std::string(where));
    }
    return text;
}

Expected<std::string> key_of(const std::filesystem::path& path, std::string_view where) {
    Expected<std::string> key = make_path_key(path);
    if (!key) {
        Error error = std::move(key.error());
        error.where = std::string(where);
        return Unexpected(std::move(error));
    }
    return key;
}

// Errors for stored values this build cannot interpret.
class RowDecoder {
public:
    RowDecoder(bool newer_schema, std::string_view where) noexcept
        : newer_schema_(newer_schema), where_(where) {}

    [[nodiscard]] Error bad(std::string_view column, std::string_view value) const {
        if (newer_schema_) {
            return Error{ErrorCode::VersionMismatch,
                         std::format("{} holds '{}', a value written by a newer Lucent", column,
                                     core::sanitize_utf8(value)),
                         std::string(where_), 0};
        }
        return Error{ErrorCode::Corrupt,
                     std::format("{} holds an invalid value '{}'", column, core::sanitize_utf8(value)),
                     std::string(where_), 0};
    }

    template <class T>
    [[nodiscard]] Expected<T> parse(std::optional<T> parsed, std::string_view column,
                                    std::string_view value) const {
        if (!parsed) {
            return Unexpected(bad(column, value));
        }
        return *parsed;
    }

    [[nodiscard]] Expected<std::filesystem::path> path(std::string_view column,
                                                       std::string_view value) const {
        Expected<std::filesystem::path> parsed = core::path_from_utf8(value);
        if (!parsed) {
            return Unexpected(bad(column, value));
        }
        return parsed;
    }

    template <std::size_t N>
    [[nodiscard]] Expected<std::array<std::uint8_t, N>> blob(std::string_view column,
                                                             std::span<const std::uint8_t> value) const {
        std::array<std::uint8_t, N> out{};
        if (value.size() != N) {
            return Unexpected(bad(column, std::format("<{} bytes>", value.size())));
        }
        std::copy(value.begin(), value.end(), out.begin());
        return out;
    }

    [[nodiscard]] Expected<std::uint32_t> u32(std::string_view column, std::int64_t value) const {
        if (value < 0 || value > std::numeric_limits<std::uint32_t>::max()) {
            return Unexpected(bad(column, std::to_string(value)));
        }
        return static_cast<std::uint32_t>(value);
    }

private:
    bool newer_schema_;
    std::string_view where_;
};

Expected<SourceRecord> read_source(const Statement& row, const RowDecoder& decode) {
    SourceRecord source;
    source.id = SourceId{row.column_int64(0)};
    Expected<std::filesystem::path> path = decode.path("sources.path", row.column_text(1));
    if (!path) {
        return Unexpected(std::move(path.error()));
    }
    source.path = std::move(*path);
    source.path_key = std::string(row.column_text(2));
    source.identity.volume_serial = detail::unsigned_from_db(row.column_int64(3));
    Expected<std::array<std::uint8_t, 16>> file_id = decode.blob<16>("sources.file_id", row.column_blob(4));
    if (!file_id) {
        return Unexpected(std::move(file_id.error()));
    }
    source.identity.file_id = *file_id;
    source.identity.size_bytes = detail::unsigned_from_db(row.column_int64(5));
    source.identity.last_write_time = row.column_int64(6);
    if (!row.is_null(7)) {
        Expected<core::Digest256> digest = decode.blob<32>("sources.sha256", row.column_blob(7));
        if (!digest) {
            return Unexpected(std::move(digest.error()));
        }
        source.sha256 = *digest;
    }
    Expected<ImportMode> mode = decode.parse(detail::parse_import_mode(row.column_text(8)),
                                             "sources.import_mode", row.column_text(8));
    if (!mode) {
        return Unexpected(std::move(mode.error()));
    }
    source.import_mode = *mode;
    if (!row.is_null(9)) {
        Expected<std::filesystem::path> origin = decode.path("sources.origin_path", row.column_text(9));
        if (!origin) {
            return Unexpected(std::move(origin.error()));
        }
        source.origin_path = std::move(*origin);
    }
    Expected<StorageKind> storage = decode.parse(detail::parse_storage_kind(row.column_text(10)),
                                                 "sources.storage_kind", row.column_text(10));
    if (!storage) {
        return Unexpected(std::move(storage.error()));
    }
    source.storage_kind = *storage;
    Expected<SourceState> state =
        decode.parse(detail::parse_source_state(row.column_text(11)), "sources.state", row.column_text(11));
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    source.state = *state;
    source.first_seen = detail::timestamp_from_db(row.column_int64(12));
    source.last_seen = detail::timestamp_from_db(row.column_int64(13));
    // The origin identity: all four columns or none.
    const int origin_nulls = static_cast<int>(row.is_null(14)) + static_cast<int>(row.is_null(15)) +
                             static_cast<int>(row.is_null(16)) + static_cast<int>(row.is_null(17));
    if (origin_nulls == 0) {
        core::FileIdentity origin;
        origin.volume_serial = detail::unsigned_from_db(row.column_int64(14));
        Expected<std::array<std::uint8_t, 16>> origin_id =
            decode.blob<16>("sources.origin_file_id", row.column_blob(15));
        if (!origin_id) {
            return Unexpected(std::move(origin_id.error()));
        }
        origin.file_id = *origin_id;
        origin.size_bytes = detail::unsigned_from_db(row.column_int64(16));
        origin.last_write_time = row.column_int64(17);
        source.origin_identity = origin;
    } else if (origin_nulls != 4) {
        return Unexpected(decode.bad("sources.origin_*", "an incomplete origin identity"));
    }
    return source;
}

Expected<ItemRecord> read_item(const Statement& row, const RowDecoder& decode) {
    ItemRecord item;
    item.id = ItemId{row.column_int64(0)};
    item.source_id = SourceId{row.column_int64(1)};
    ItemMetadata& m = item.metadata;
    Expected<std::uint32_t> subimage = decode.u32("items.subimage_index", row.column_int64(2));
    if (!subimage) {
        return Unexpected(std::move(subimage.error()));
    }
    m.subimage_index = *subimage;
    m.make = std::string(row.column_text(3));
    m.model = std::string(row.column_text(4));
    m.lens = std::string(row.column_text(5));
    if (!row.is_null(6)) {
        m.capture_time = std::string(row.column_text(6));
    }
    Expected<std::uint32_t> width = decode.u32("items.width", row.column_int64(7));
    Expected<std::uint32_t> height = decode.u32("items.height", row.column_int64(8));
    if (!width || !height) {
        return Unexpected(!width ? std::move(width.error()) : std::move(height.error()));
    }
    m.width = *width;
    m.height = *height;
    const std::int64_t orientation = row.column_int64(9);
    if (orientation < 1 || orientation > 8) {
        return Unexpected(decode.bad("items.orientation", std::to_string(orientation)));
    }
    m.orientation = static_cast<std::uint8_t>(orientation);
    m.format = std::string(row.column_text(10));
    Expected<SupportStatus> support = decode.parse(detail::parse_support_status(row.column_text(11)),
                                                   "items.support_status", row.column_text(11));
    if (!support) {
        return Unexpected(std::move(support.error()));
    }
    m.support_status = *support;
    m.support_reasons_json = std::string(row.column_text(12));
    m.decoder_id = std::string(row.column_text(13));
    m.decoder_version = std::string(row.column_text(14));
    m.probed_at = detail::timestamp_from_db(row.column_int64(15));
    Expected<PickFlag> pick =
        decode.parse(detail::parse_pick_flag(row.column_text(16)), "items.pick", row.column_text(16));
    if (!pick) {
        return Unexpected(std::move(pick.error()));
    }
    item.flags.pick = *pick;
    const std::int64_t rating = row.column_int64(17);
    if (rating < 0 || rating > 5) {
        return Unexpected(decode.bad("items.rating", std::to_string(rating)));
    }
    item.flags.rating = static_cast<std::uint8_t>(rating);
    Expected<std::filesystem::path> path = decode.path("sources.path", row.column_text(18));
    if (!path) {
        return Unexpected(std::move(path.error()));
    }
    item.source_path = std::move(*path);
    Expected<SourceState> state =
        decode.parse(detail::parse_source_state(row.column_text(19)), "sources.state", row.column_text(19));
    if (!state) {
        return Unexpected(std::move(state.error()));
    }
    item.source_state = *state;
    Expected<StorageKind> storage = decode.parse(detail::parse_storage_kind(row.column_text(20)),
                                                 "sources.storage_kind", row.column_text(20));
    if (!storage) {
        return Unexpected(std::move(storage.error()));
    }
    item.storage_kind = *storage;
    item.source_hashed = row.column_int64(21) != 0;
    if (!row.is_null(23)) {
        item.current_revision = RevisionId{row.column_int64(23)};
    }
    struct CaptureColumn {
        int column;
        std::string_view name;
        std::optional<double>* field;
    };
    const CaptureColumn capture[] = {{24, "items.iso", &m.iso},
                                     {25, "items.exposure_time_s", &m.exposure_time_s},
                                     {26, "items.f_number", &m.f_number},
                                     {27, "items.focal_length_mm", &m.focal_length_mm}};
    for (const CaptureColumn& c : capture) {
        if (row.is_null(c.column)) {
            continue;
        }
        const double value = row.column_double(c.column);
        if (!std::isfinite(value) || value <= 0.0) {
            return Unexpected(decode.bad(c.name, std::format("{}", value)));
        }
        *c.field = value;
    }
    if (!row.is_null(28)) {
        const std::int64_t count = row.column_int64(28);
        if (count < 1 || count > std::numeric_limits<std::uint32_t>::max()) {
            return Unexpected(decode.bad("items.image_count", std::to_string(count)));
        }
        m.image_count = static_cast<std::uint32_t>(count);
    }
    return item;
}

Expected<RevisionInfo> read_revision_info(const Statement& row, const RowDecoder& decode) {
    RevisionInfo info;
    info.id = RevisionId{row.column_int64(0)};
    info.item = ItemId{row.column_int64(1)};
    Expected<std::uint32_t> number = decode.u32("recipe_revisions.revision_number", row.column_int64(2));
    if (!number) {
        return Unexpected(std::move(number.error()));
    }
    info.revision_number = *number;
    if (!row.is_null(3)) {
        info.parent = RevisionId{row.column_int64(3)};
    }
    info.created_at = detail::timestamp_from_db(row.column_int64(4));
    // An origin a later Lucent added labels the history only: listed as Unknown, never an error
    // (one such revision must not make the item's history, or its current revision, unreadable).
    info.origin = detail::parse_revision_origin(row.column_text(5)).value_or(RevisionOrigin::Unknown);
    info.schema_name = std::string(row.column_text(6));
    Expected<std::uint32_t> schema_version =
        decode.u32("recipe_revisions.schema_version", row.column_int64(7));
    if (!schema_version) {
        return Unexpected(std::move(schema_version.error()));
    }
    info.schema_version = *schema_version;
    Expected<core::Digest256> digest =
        decode.blob<32>("recipe_revisions.settings_sha256", row.column_blob(8));
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    info.settings_sha256 = *digest;
    info.settings_bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, row.column_int64(9)));
    info.engine_version = std::string(row.column_text(10));
    info.note = std::string(row.column_text(11));
    return info;
}

Expected<core::Digest256> sha256_of(std::string_view text) {
    return core::sha256(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

// A revision with its settings document, whose hash is verified.
Expected<RecipeRevision> read_revision(const Statement& row, const RowDecoder& decode,
                                       std::string_view where) {
    Expected<RevisionInfo> info = read_revision_info(row, decode);
    if (!info) {
        return Unexpected(std::move(info.error()));
    }
    RecipeRevision revision;
    revision.info = std::move(*info);
    revision.settings_json = std::string(row.column_text(kRevisionSettingsColumn));
    Expected<core::Digest256> digest = sha256_of(revision.settings_json);
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }
    if (*digest != revision.info.settings_sha256 ||
        revision.settings_json.size() != revision.info.settings_bytes) {
        return fail(ErrorCode::Corrupt,
                    std::format("the settings of recipe revision {} do not match their SHA-256",
                                revision.info.id.value),
                    std::string(where));
    }
    return revision;
}

template <class Record, class Reader>
Expected<std::vector<Record>> collect(Statement& statement, const RowDecoder& decode, Reader reader) {
    std::vector<Record> records;
    for (;;) {
        Expected<bool> row = statement.step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return records;
        }
        Expected<Record> record = reader(statement, decode);
        if (!record) {
            return Unexpected(std::move(record.error()));
        }
        records.push_back(std::move(*record));
    }
}

template <class Record, class Reader>
Expected<std::optional<Record>> first(Statement& statement, const RowDecoder& decode, Reader reader) {
    Expected<bool> row = statement.step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return std::optional<Record>();
    }
    Expected<Record> record = reader(statement, decode);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    return std::optional<Record>(std::move(*record));
}

// Builds the WHERE clause of list_items() / count_items(); returns InvalidArgument for bad filters.
Status item_filter_clause(const ItemFilter& filter, std::string& where_sql,
                          std::vector<Parameter>& parameters, std::string_view where) {
    const auto in_list = [&](std::string_view column, const auto& values, auto to_text,
                             std::string_view field) -> Status {
        if (values.empty()) {
            return ok();
        }
        std::string clause = std::format(" AND {} IN (", column);
        bool first_value = true;
        for (const auto value : values) {
            const std::string_view text = to_text(value);
            if (text == "invalid") {
                return invalid(where, std::format("unknown {} in the filter", field));
            }
            clause += first_value ? "?" : ", ?";
            first_value = false;
            parameters.emplace_back(std::string(text));
        }
        clause += ")";
        where_sql += clause;
        return ok();
    };
    if (Status s = in_list(
            "s.state", filter.source_states, [](SourceState v) { return to_string(v); }, "source state");
        !s) {
        return s;
    }
    if (Status s = in_list(
            "i.support_status", filter.support, [](SupportStatus v) { return to_string(v); },
            "support status");
        !s) {
        return s;
    }
    if (filter.duplicates_only) {
        where_sql +=
            " AND s.sha256 IS NOT NULL AND EXISTS (SELECT 1 FROM sources d WHERE d.sha256 = s.sha256 AND "
            "d.id <> s.id)";
    }
    if (filter.source) {
        where_sql += " AND i.source_id = ?";
        parameters.emplace_back(filter.source->value);
    }
    if (filter.pick) {
        const std::string_view text = to_string(*filter.pick);
        if (text == "invalid") {
            return invalid(where, "unknown pick flag in the filter");
        }
        where_sql += " AND i.pick = ?";
        parameters.emplace_back(std::string(text));
    }
    if (filter.min_rating) {
        if (*filter.min_rating < 1 || *filter.min_rating > 5) {
            return invalid(where, "min_rating must be 1 ... 5");
        }
        where_sql += " AND i.rating >= ?";
        parameters.emplace_back(static_cast<std::int64_t>(*filter.min_rating));
    }
    return ok();
}

Status require_changed(Connection& connection, std::string_view what, std::int64_t id,
                       std::string_view where) {
    if (connection.changes() == 0) {
        return fail(ErrorCode::NotFound, std::format("no {} with id {}", what, id), std::string(where));
    }
    return ok();
}

Status validate_item(const ItemMetadata& item, std::string_view where) {
    for (const auto& [text, field] : {std::pair<std::string_view, std::string_view>{item.make, "make"},
                                      {item.model, "model"},
                                      {item.lens, "lens"},
                                      {item.format, "format"},
                                      {item.support_reasons_json, "support_reasons_json"},
                                      {item.decoder_id, "decoder_id"},
                                      {item.decoder_version, "decoder_version"}}) {
        if (Status s = check_text(text, field, where); !s) {
            return s;
        }
    }
    if (item.capture_time && !detail::is_valid_capture_time(*item.capture_time)) {
        return invalid(where, "capture_time must have the form YYYY-MM-DDTHH:MM:SS");
    }
    if (item.orientation < 1 || item.orientation > 8) {
        return invalid(where, "orientation must be an EXIF orientation 1 ... 8");
    }
    if (!detail::is_valid(item.support_status)) {
        return invalid(where, "unknown support status");
    }
    const std::pair<const std::optional<double>*, std::string_view> capture[] = {
        {&item.iso, "iso"},
        {&item.exposure_time_s, "exposure_time_s"},
        {&item.f_number, "f_number"},
        {&item.focal_length_mm, "focal_length_mm"}};
    for (const auto& [value, field] : capture) {
        if (value->has_value() && (!std::isfinite(**value) || **value <= 0.0)) {
            return invalid(where, std::format("{} must be finite and positive", field));
        }
    }
    if (item.image_count && (*item.image_count < 1 || item.subimage_index >= *item.image_count)) {
        return invalid(where, "image_count must be at least 1 and greater than subimage_index");
    }
    return ok();
}

Expected<std::int64_t> count_to_db(std::uint64_t value, std::string_view where) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(ErrorCode::InvalidArgument, "count too large", std::string(where));
    }
    return static_cast<std::int64_t>(value);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ReadTransaction
// ---------------------------------------------------------------------------------------------

namespace {

// Every call runs inside the transaction of read() / write(). SQLite rolls a transaction back on
// its own after some errors (I/O, disk full, out of memory); a later call would then run in
// autocommit mode and commit on its own, so it is refused instead.
Status check_active(const Connection* connection, std::string_view where) {
    if (connection == nullptr) {
        return fail(ErrorCode::Internal, "the catalog transaction is no longer active", std::string(where));
    }
    if (!connection->in_transaction()) {
        return fail(ErrorCode::IoError, "the catalog transaction was rolled back after an earlier error",
                    std::string(where));
    }
    return ok();
}

}  // namespace

Expected<std::optional<SourceRecord>> ReadTransaction::find_source(SourceId id) const {
    constexpr std::string_view where = "catalog.find_source";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement =
        connection_->prepare(sql({"SELECT ", kSourceColumns, " FROM sources s WHERE s.id = ?1"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, id.value);
    return first<SourceRecord>(*statement, RowDecoder(newer_schema_, where), read_source);
}

Expected<std::optional<SourceRecord>> ReadTransaction::find_source_by_path(
    const std::filesystem::path& path) const {
    constexpr std::string_view where = "catalog.find_source_by_path";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::string> key = key_of(path, where);
    if (!key) {
        return Unexpected(std::move(key.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kSourceColumns, " FROM sources s WHERE s.path_key = ?1"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_text(1, *key);
    return first<SourceRecord>(*statement, RowDecoder(newer_schema_, where), read_source);
}

Expected<std::vector<SourceRecord>> ReadTransaction::find_sources_by_file_id(
    std::uint64_t volume_serial, const std::array<std::uint8_t, 16>& file_id) const {
    constexpr std::string_view where = "catalog.find_sources_by_file_id";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kSourceColumns,
             " FROM sources s WHERE s.volume_serial = ?1 AND s.file_id = ?2 ORDER BY s.id"}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, detail::to_db(volume_serial)).bind_blob(2, bytes_of(file_id));
    return collect<SourceRecord>(*statement, RowDecoder(newer_schema_, where), read_source);
}

Expected<std::vector<SourceRecord>> ReadTransaction::find_sources_by_sha256(
    const core::Digest256& sha256) const {
    constexpr std::string_view where = "catalog.find_sources_by_sha256";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kSourceColumns, " FROM sources s WHERE s.sha256 = ?1 ORDER BY s.id"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_blob(1, bytes_of(sha256));
    return collect<SourceRecord>(*statement, RowDecoder(newer_schema_, where), read_source);
}

Expected<std::vector<SourceRecord>> ReadTransaction::list_sources(const SourceQuery& query) const {
    constexpr std::string_view where = "catalog.list_sources";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(query.limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::string text = sql({"SELECT ", kSourceColumns, " FROM sources s WHERE s.id > ?"});
    std::vector<Parameter> parameters{query.after.value};
    if (!query.states.empty()) {
        text += " AND s.state IN (";
        for (std::size_t i = 0; i < query.states.size(); ++i) {
            const std::string_view state = to_string(query.states[i]);
            if (state == "invalid") {
                return fail(ErrorCode::InvalidArgument, "unknown source state in the query",
                            std::string(where));
            }
            text += i == 0 ? "?" : ", ?";
            parameters.emplace_back(std::string(state));
        }
        text += ")";
    }
    text += " ORDER BY s.id LIMIT ?";
    parameters.emplace_back(static_cast<std::int64_t>(query.limit));
    Expected<Statement> statement = connection_->prepare(text, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    bind_all(*statement, parameters);
    return collect<SourceRecord>(*statement, RowDecoder(newer_schema_, where), read_source);
}

Expected<std::uint64_t> ReadTransaction::count_sources() const {
    constexpr std::string_view where = "catalog.count_sources";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::int64_t> count = connection_->query_int64("SELECT count(*) FROM sources", where);
    if (!count) {
        return Unexpected(std::move(count.error()));
    }
    return static_cast<std::uint64_t>(*count);
}

Expected<std::optional<ItemRecord>> ReadTransaction::find_item(ItemId id) const {
    constexpr std::string_view where = "catalog.find_item";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kItemColumns, " FROM items i JOIN sources s ON s.id = i.source_id WHERE i.id = ?1"}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, id.value);
    return first<ItemRecord>(*statement, RowDecoder(newer_schema_, where), read_item);
}

Expected<std::vector<ItemRecord>> ReadTransaction::items_of_source(SourceId source) const {
    constexpr std::string_view where = "catalog.items_of_source";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement =
        connection_->prepare(sql({"SELECT ", kItemColumns,
                                  " FROM items i JOIN sources s ON s.id = i.source_id WHERE i.source_id = ?1 "
                                  "ORDER BY i.subimage_index"}),
                             where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, source.value);
    return collect<ItemRecord>(*statement, RowDecoder(newer_schema_, where), read_item);
}

Expected<ItemPage> ReadTransaction::list_items(const ItemQuery& query) const {
    constexpr std::string_view where = "catalog.list_items";
    using detail::ItemCursorAccess;
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(query.limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (query.order != ItemOrder::Path && query.order != ItemOrder::CaptureTime &&
        query.order != ItemOrder::Added) {
        return fail(ErrorCode::InvalidArgument, "unknown item order", std::string(where));
    }
    std::string filter_sql;
    std::vector<Parameter> parameters;
    if (Status s = item_filter_clause(query.filter, filter_sql, parameters, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (query.after) {
        const ItemCursor& cursor = *query.after;
        if (ItemCursorAccess::order(cursor) != query.order) {
            return fail(ErrorCode::InvalidArgument, "the cursor belongs to another item order",
                        std::string(where));
        }
        switch (query.order) {
            case ItemOrder::Path:
                // The first, redundant term lets SQLite seek sources_by_path_key; the row value
                // spans two tables and cannot drive an index on its own.
                filter_sql += " AND s.path_key >= ? AND (s.path_key, i.subimage_index) > (?, ?)";
                parameters.emplace_back(ItemCursorAccess::text(cursor));
                parameters.emplace_back(ItemCursorAccess::text(cursor));
                parameters.emplace_back(ItemCursorAccess::number(cursor));
                break;
            case ItemOrder::CaptureTime:
                // Same expressions as items_by_capture_time; the redundant first term lets SQLite
                // seek in that index.
                filter_sql +=
                    " AND ifnull(i.capture_time, '~') >= ? AND (ifnull(i.capture_time, '~'), i.id) > (?, ?)";
                parameters.emplace_back(ItemCursorAccess::text(cursor));
                parameters.emplace_back(ItemCursorAccess::text(cursor));
                parameters.emplace_back(ItemCursorAccess::number(cursor));
                break;
            case ItemOrder::Added:
                filter_sql += " AND i.id > ?";
                parameters.emplace_back(ItemCursorAccess::number(cursor));
                break;
        }
    }
    std::string_view order_sql;
    switch (query.order) {
        case ItemOrder::Path:
            order_sql = " ORDER BY s.path_key, i.subimage_index";
            break;
        case ItemOrder::CaptureTime:
            order_sql = " ORDER BY ifnull(i.capture_time, '~'), i.id";
            break;
        case ItemOrder::Added:
            order_sql = " ORDER BY i.id";
            break;
    }
    // One extra row tells whether another page follows.
    parameters.emplace_back(static_cast<std::int64_t>(query.limit) + 1);
    const std::string text =
        sql({"SELECT ", kItemColumns, " FROM items i JOIN sources s ON s.id = i.source_id WHERE 1",
             filter_sql, order_sql, " LIMIT ?"});
    Expected<Statement> statement = connection_->prepare(text, where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    bind_all(*statement, parameters);

    ItemPage page;
    const RowDecoder decode(newer_schema_, where);
    std::string last_path_key;
    for (;;) {
        Expected<bool> row = statement->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        if (page.items.size() == query.limit) {
            const ItemRecord& last = page.items.back();
            switch (query.order) {
                case ItemOrder::Path:
                    page.next =
                        ItemCursorAccess::make(ItemOrder::Path, last_path_key,
                                               static_cast<std::int64_t>(last.metadata.subimage_index));
                    break;
                case ItemOrder::CaptureTime:
                    page.next = ItemCursorAccess::make(
                        ItemOrder::CaptureTime, last.metadata.capture_time.value_or(kUnknownCaptureTime),
                        last.id.value);
                    break;
                case ItemOrder::Added:
                    page.next = ItemCursorAccess::make(ItemOrder::Added, {}, last.id.value);
                    break;
            }
            break;
        }
        Expected<ItemRecord> item = read_item(*statement, decode);
        if (!item) {
            return Unexpected(std::move(item.error()));
        }
        last_path_key = std::string(statement->column_text(kItemPathKeyColumn));
        page.items.push_back(std::move(*item));
    }
    return page;
}

Expected<std::uint64_t> ReadTransaction::count_items(const ItemFilter& filter) const {
    constexpr std::string_view where = "catalog.count_items";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::string filter_sql;
    std::vector<Parameter> parameters;
    if (Status s = item_filter_clause(filter, filter_sql, parameters, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT count(*) FROM items i JOIN sources s ON s.id = i.source_id WHERE 1", filter_sql}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    bind_all(*statement, parameters);
    Expected<bool> row = statement->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    return static_cast<std::uint64_t>(statement->column_int64(0));
}

namespace {

Expected<ImportRecord> read_import(Connection& connection, const Statement& row, const RowDecoder& decode,
                                   std::string_view where) {
    ImportRecord record;
    record.id = ImportId{row.column_int64(0)};
    record.started_at = detail::timestamp_from_db(row.column_int64(1));
    if (!row.is_null(2)) {
        record.finished_at = detail::timestamp_from_db(row.column_int64(2));
    }
    Expected<ImportOutcome> outcome =
        decode.parse(detail::parse_import_outcome(row.column_text(3)), "imports.outcome", row.column_text(3));
    if (!outcome) {
        return Unexpected(std::move(outcome.error()));
    }
    record.outcome = *outcome;
    const std::string roots_json(row.column_text(4));
    record.options_json = std::string(row.column_text(5));
    std::uint64_t* const counts[] = {
        &record.counts.files_seen, &record.counts.added,   &record.counts.refreshed, &record.counts.unchanged,
        &record.counts.duplicates, &record.counts.skipped, &record.counts.failed};
    for (int i = 0; i < 7; ++i) {
        *counts[i] = static_cast<std::uint64_t>(row.column_int64(6 + i));
    }

    Expected<Statement> roots = connection.prepare(
        "SELECT json_extract(r.value, '$.path'), json_extract(r.value, '$.include_subfolders') "
        "FROM json_each(?1) AS r ORDER BY r.key",
        where);
    if (!roots) {
        return Unexpected(std::move(roots.error()));
    }
    roots->bind_text(1, roots_json);
    for (;;) {
        Expected<bool> next = roots->step();
        if (!next) {
            return Unexpected(std::move(next.error()));
        }
        if (!*next) {
            break;
        }
        Expected<std::filesystem::path> path = decode.path("imports.roots_json", roots->column_text(0));
        if (!path) {
            return Unexpected(std::move(path.error()));
        }
        record.roots.push_back(ImportRoot{std::move(*path), roots->column_int64(1) != 0});
    }
    return record;
}

}  // namespace

Expected<std::optional<ImportRecord>> ReadTransaction::find_import(ImportId id) const {
    constexpr std::string_view where = "catalog.find_import";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement =
        connection_->prepare(sql({"SELECT ", kImportColumns, " FROM imports WHERE id = ?1"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, id.value);
    Connection& connection = *connection_;
    return first<ImportRecord>(*statement, RowDecoder(newer_schema_, where),
                               [&](const Statement& row, const RowDecoder& decode) {
                                   return read_import(connection, row, decode, where);
                               });
}

Expected<std::vector<ImportRecord>> ReadTransaction::list_imports(std::uint32_t limit) const {
    constexpr std::string_view where = "catalog.list_imports";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_limit(limit, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kImportColumns, " FROM imports ORDER BY id DESC LIMIT ?1"}), where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, static_cast<std::int64_t>(limit));
    Connection& connection = *connection_;
    return collect<ImportRecord>(*statement, RowDecoder(newer_schema_, where),
                                 [&](const Statement& row, const RowDecoder& decode) {
                                     return read_import(connection, row, decode, where);
                                 });
}

// ---------------------------------------------------------------------------------------------
// WriteTransaction
// ---------------------------------------------------------------------------------------------

Expected<SourceUpsertResult> WriteTransaction::upsert_source(const SourceObservation& observation) {
    constexpr std::string_view where = "catalog.upsert_source";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::string> key = key_of(observation.path, where);
    if (!key) {
        return Unexpected(std::move(key.error()));
    }
    Expected<std::string> path = path_text(observation.path, where);
    if (!path) {
        return Unexpected(std::move(path.error()));
    }
    if (!detail::is_valid(observation.import_mode) || !detail::is_valid(observation.storage_kind)) {
        return fail(ErrorCode::InvalidArgument, "unknown import mode or storage kind", std::string(where));
    }
    std::optional<std::string> origin;
    if (observation.import_mode == ImportMode::Copied) {
        if (!observation.origin_path) {
            return fail(ErrorCode::InvalidArgument, "a copied source needs its origin path",
                        std::string(where));
        }
        Expected<std::string> text = path_text(*observation.origin_path, where);
        if (!text) {
            return Unexpected(std::move(text.error()));
        }
        origin = std::move(*text);
    } else if (observation.origin_path || observation.origin_identity) {
        return fail(ErrorCode::InvalidArgument, "only copied sources have an origin path or identity",
                    std::string(where));
    }
    // Binds the origin identity (or NULLs) to parameters first ... first + 3.
    const auto bind_origin = [&observation](Statement& statement, int first) {
        if (const std::optional<core::FileIdentity>& origin = observation.origin_identity) {
            statement.bind_int64(first, detail::to_db(origin->volume_serial))
                .bind_blob(first + 1, bytes_of(origin->file_id))
                .bind_int64(first + 2, detail::to_db(origin->size_bytes))
                .bind_int64(first + 3, origin->last_write_time);
        } else {
            statement.bind_null(first).bind_null(first + 1).bind_null(first + 2).bind_null(first + 3);
        }
    };

    Expected<Statement> lookup = connection_->prepare(
        "SELECT id, volume_serial, file_id, size, mtime, sha256, state FROM sources WHERE path_key = ?1",
        where);
    if (!lookup) {
        return Unexpected(std::move(lookup.error()));
    }
    lookup->bind_text(1, *key);
    Expected<bool> found = lookup->step();
    if (!found) {
        return Unexpected(std::move(found.error()));
    }

    const core::FileIdentity& identity = observation.identity;
    if (!*found) {
        lookup->reset();
        Expected<Statement> insert = connection_->prepare(
            "INSERT INTO sources (path, path_key, volume_serial, file_id, size, mtime, sha256, import_mode, "
            "origin_path, storage_kind, state, first_seen, last_seen, origin_volume_serial, origin_file_id, "
            "origin_size, origin_mtime) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 'present', ?11, ?11, ?12, ?13, ?14, ?15) "
            "RETURNING id",
            where);
        if (!insert) {
            return Unexpected(std::move(insert.error()));
        }
        insert->bind_text(1, *path)
            .bind_text(2, *key)
            .bind_int64(3, detail::to_db(identity.volume_serial))
            .bind_blob(4, bytes_of(identity.file_id))
            .bind_int64(5, detail::to_db(identity.size_bytes))
            .bind_int64(6, identity.last_write_time)
            .bind_text(8, to_string(observation.import_mode))
            .bind_optional_text(9, origin)
            .bind_text(10, detail::storage_kind_text(observation.storage_kind))
            .bind_int64(11, detail::to_db(observation.seen_at));
        bind_origin(*insert, 12);
        if (observation.sha256) {
            insert->bind_blob(7, bytes_of(*observation.sha256));
        } else {
            insert->bind_null(7);
        }
        Expected<bool> row = insert->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return fail(ErrorCode::Internal, "INSERT ... RETURNING returned no row", std::string(where));
        }
        return SourceUpsertResult{SourceId{insert->column_int64(0)}, SourceUpsertOutcome::Inserted};
    }

    // An existing source: compare the stored identity with the observation.
    const SourceId id{lookup->column_int64(0)};
    const bool same_object =
        detail::unsigned_from_db(lookup->column_int64(1)) == identity.volume_serial &&
        lookup->column_blob(2).size() == identity.file_id.size() &&
        std::equal(identity.file_id.begin(), identity.file_id.end(), lookup->column_blob(2).begin());
    const bool same_version = detail::unsigned_from_db(lookup->column_int64(3)) == identity.size_bytes &&
                              lookup->column_int64(4) == identity.last_write_time;
    bool hashes_agree = true;
    if (observation.sha256 && !lookup->is_null(5)) {
        const std::span<const std::uint8_t> stored = lookup->column_blob(5);
        hashes_agree = stored.size() == observation.sha256->size() &&
                       std::equal(stored.begin(), stored.end(), observation.sha256->begin());
    }
    const std::string previous_state(lookup->column_text(6));
    lookup->reset();

    const bool same = same_object && same_version && hashes_agree;
    // When the file is unchanged, an absent observed hash keeps the stored one (COALESCE); when it
    // changed, the stored hash describes the old content and is replaced, possibly by NULL.
    Expected<Statement> update = connection_->prepare(
        same ? std::string_view(
                   "UPDATE sources SET path = ?2, sha256 = coalesce(?3, sha256), import_mode = ?4, "
                   "origin_path = ?5, storage_kind = ?6, state = 'present', last_seen = ?7, "
                   "origin_volume_serial = ?12, origin_file_id = ?13, origin_size = ?14, origin_mtime = ?15 "
                   "WHERE id = ?1")
             : std::string_view(
                   "UPDATE sources SET path = ?2, sha256 = ?3, import_mode = ?4, origin_path = ?5, "
                   "storage_kind = ?6, state = 'present', last_seen = ?7, volume_serial = ?8, "
                   "file_id = ?9, size = ?10, mtime = ?11, origin_volume_serial = ?12, origin_file_id = ?13, "
                   "origin_size = ?14, origin_mtime = ?15 WHERE id = ?1"),
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, id.value)
        .bind_text(2, *path)
        .bind_text(4, to_string(observation.import_mode))
        .bind_optional_text(5, origin)
        .bind_text(6, detail::storage_kind_text(observation.storage_kind))
        .bind_int64(7, detail::to_db(observation.seen_at));
    bind_origin(*update, 12);
    if (observation.sha256) {
        update->bind_blob(3, bytes_of(*observation.sha256));
    } else {
        update->bind_null(3);
    }
    if (!same) {
        update->bind_int64(8, detail::to_db(identity.volume_serial))
            .bind_blob(9, bytes_of(identity.file_id))
            .bind_int64(10, detail::to_db(identity.size_bytes))
            .bind_int64(11, identity.last_write_time);
    }
    if (Status s = update->run(); !s) {
        return Unexpected(std::move(s.error()));
    }
    SourceUpsertOutcome outcome = SourceUpsertOutcome::Replaced;
    if (same) {
        outcome = previous_state == to_string(SourceState::Present) ? SourceUpsertOutcome::Unchanged
                                                                    : SourceUpsertOutcome::Restored;
    }
    return SourceUpsertResult{id, outcome};
}

Status WriteTransaction::set_source_hash(SourceId id, const core::Digest256& sha256,
                                         const core::FileIdentity& hashed) {
    constexpr std::string_view where = "catalog.set_source_hash";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    Expected<Statement> update = connection_->prepare(
        "UPDATE sources SET sha256 = ?2 WHERE id = ?1 AND volume_serial = ?3 AND file_id = ?4 AND size = ?5 "
        "AND "
        "mtime = ?6",
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, id.value)
        .bind_blob(2, bytes_of(sha256))
        .bind_int64(3, detail::to_db(hashed.volume_serial))
        .bind_blob(4, bytes_of(hashed.file_id))
        .bind_int64(5, detail::to_db(hashed.size_bytes))
        .bind_int64(6, hashed.last_write_time);
    if (Status s = update->run(); !s) {
        return s;
    }
    if (connection_->changes() > 0) {
        return ok();
    }
    Expected<std::optional<SourceRecord>> source = find_source(id);
    if (!source) {
        return Unexpected(std::move(source.error()));
    }
    if (!source->has_value()) {
        return fail(ErrorCode::NotFound, std::format("no source with id {}", id.value), std::string(where));
    }
    return fail(ErrorCode::SourceChanged,
                "the hashed file is not the catalogued version of the source (identity differs); reimport it",
                std::string(where));
}

Status WriteTransaction::set_source_state(SourceId id, SourceState state, Timestamp checked_at) {
    constexpr std::string_view where = "catalog.set_source_state";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    if (!detail::is_valid(state)) {
        return fail(ErrorCode::InvalidArgument, "unknown source state", std::string(where));
    }
    Expected<Statement> update = connection_->prepare(
        "UPDATE sources SET state = ?2, last_seen = CASE WHEN ?2 = 'present' THEN ?3 ELSE last_seen END "
        "WHERE id = ?1",
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, id.value).bind_text(2, to_string(state)).bind_int64(3, detail::to_db(checked_at));
    if (Status s = update->run(); !s) {
        return s;
    }
    return require_changed(*connection_, "source", id.value, where);
}

Expected<SourceRecord> WriteTransaction::relink_source(SourceId id, const RelinkRequest& request) {
    constexpr std::string_view where = "catalog.relink_source";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::string> key = key_of(request.new_path, where);
    if (!key) {
        return Unexpected(std::move(key.error()));
    }
    Expected<std::string> path = path_text(request.new_path, where);
    if (!path) {
        return Unexpected(std::move(path.error()));
    }
    if (!detail::is_valid(request.storage_kind)) {
        return fail(ErrorCode::InvalidArgument, "unknown storage kind", std::string(where));
    }
    Expected<std::optional<SourceRecord>> current = find_source(id);
    if (!current) {
        return Unexpected(std::move(current.error()));
    }
    if (!current->has_value()) {
        return fail(ErrorCode::NotFound, std::format("no source with id {}", id.value), std::string(where));
    }
    const SourceRecord& source = **current;
    // Verified by content (both hashes known and equal) or by identity (the catalogued version of
    // the file object itself); a contradicting hash is never accepted.
    if (source.sha256 && request.sha256 && *source.sha256 != *request.sha256) {
        return fail(ErrorCode::InvalidArgument,
                    "the file at '" + core::path_to_utf8_lossy(request.new_path) +
                        "' has different content than the catalogued source",
                    "catalog.relink.content_mismatch");
    }
    const bool verified = (source.sha256 && request.sha256) || source.identity == request.identity;
    if (!verified && !request.allow_unverified) {
        return fail(ErrorCode::InvalidArgument,
                    source.sha256
                        ? "the new file was not hashed and is not the catalogued file version, so it "
                          "cannot be verified (allow_unverified not set)"
                        : "the source was never hashed and the new file is not the catalogued file "
                          "version, so it cannot be verified (allow_unverified not set)",
                    "catalog.relink.unverified");
    }
    {
        Expected<Statement> clash =
            connection_->prepare("SELECT id FROM sources WHERE path_key = ?1 AND id <> ?2", where);
        if (!clash) {
            return Unexpected(std::move(clash.error()));
        }
        clash->bind_text(1, *key).bind_int64(2, id.value);
        Expected<bool> taken = clash->step();
        if (!taken) {
            return Unexpected(std::move(taken.error()));
        }
        if (*taken) {
            return fail(ErrorCode::AlreadyExists,
                        std::format("source {} already has the path '{}'", clash->column_int64(0),
                                    core::path_to_utf8_lossy(request.new_path)),
                        std::string(where));
        }
    }
    Expected<Statement> update = connection_->prepare(
        "UPDATE sources SET path = ?2, path_key = ?3, volume_serial = ?4, file_id = ?5, size = ?6, mtime = "
        "?7, sha256 = coalesce(?8, sha256), storage_kind = ?9, state = 'present', last_seen = ?10 "
        "WHERE id = ?1",
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, id.value)
        .bind_text(2, *path)
        .bind_text(3, *key)
        .bind_int64(4, detail::to_db(request.identity.volume_serial))
        .bind_blob(5, bytes_of(request.identity.file_id))
        .bind_int64(6, detail::to_db(request.identity.size_bytes))
        .bind_int64(7, request.identity.last_write_time)
        .bind_text(9, detail::storage_kind_text(request.storage_kind))
        .bind_int64(10, detail::to_db(request.seen_at));
    if (request.sha256) {
        update->bind_blob(8, bytes_of(*request.sha256));
    } else {
        update->bind_null(8);  // keeps the stored hash (identity-verified move)
    }
    if (Status s = update->run(); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::optional<SourceRecord>> updated = find_source(id);
    if (!updated) {
        return Unexpected(std::move(updated.error()));
    }
    if (!updated->has_value()) {
        return fail(ErrorCode::Internal, "relinked source vanished", std::string(where));
    }
    return std::move(**updated);
}

Status WriteTransaction::remove_source(SourceId id) {
    constexpr std::string_view where = "catalog.remove_source";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    Expected<Statement> remove = connection_->prepare("DELETE FROM sources WHERE id = ?1", where);
    if (!remove) {
        return Unexpected(std::move(remove.error()));
    }
    remove->bind_int64(1, id.value);
    if (Status s = remove->run(); !s) {
        return s;
    }
    return require_changed(*connection_, "source", id.value, where);
}

Expected<std::vector<ItemId>> WriteTransaction::upsert_items(SourceId source,
                                                             std::span<const ItemMetadata> items) {
    constexpr std::string_view where = "catalog.upsert_items";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::set<std::uint32_t> subimages;
    for (const ItemMetadata& item : items) {
        if (Status s = validate_item(item, where); !s) {
            return Unexpected(std::move(s.error()));
        }
        if (!subimages.insert(item.subimage_index).second) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("subimage {} is listed twice", item.subimage_index), std::string(where));
        }
    }
    {
        Expected<Statement> exists = connection_->prepare("SELECT 1 FROM sources WHERE id = ?1", where);
        if (!exists) {
            return Unexpected(std::move(exists.error()));
        }
        exists->bind_int64(1, source.value);
        Expected<bool> row = exists->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return fail(ErrorCode::NotFound, std::format("no source with id {}", source.value),
                        std::string(where));
        }
    }
    Expected<TransactionGuard> savepoint =
        TransactionGuard::begin(*connection_, TransactionGuard::Kind::Savepoint, where);
    if (!savepoint) {
        return Unexpected(std::move(savepoint.error()));
    }
    std::vector<ItemId> ids;
    ids.reserve(items.size());
    for (const ItemMetadata& item : items) {
        Expected<Statement> upsert = connection_->prepare(
            "INSERT INTO items (source_id, subimage_index, make, model, lens, capture_time, width, height, "
            "orientation, format, support_status, support_reasons_json, decoder_id, decoder_version, "
            "probed_at, iso, exposure_time_s, f_number, focal_length_mm, image_count) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, "
            "?20) "
            "ON CONFLICT (source_id, subimage_index) DO UPDATE SET make = excluded.make, model = "
            "excluded.model, "
            "lens = excluded.lens, capture_time = excluded.capture_time, width = excluded.width, "
            "height = excluded.height, orientation = excluded.orientation, format = excluded.format, "
            "support_status = excluded.support_status, support_reasons_json = excluded.support_reasons_json, "
            "decoder_id = excluded.decoder_id, decoder_version = excluded.decoder_version, "
            "probed_at = excluded.probed_at, iso = excluded.iso, exposure_time_s = excluded.exposure_time_s, "
            "f_number = excluded.f_number, focal_length_mm = excluded.focal_length_mm, "
            "image_count = excluded.image_count "
            "RETURNING id",
            where);
        if (!upsert) {
            return Unexpected(std::move(upsert.error()));
        }
        upsert->bind_int64(1, source.value)
            .bind_int64(2, static_cast<std::int64_t>(item.subimage_index))
            .bind_text(3, item.make)
            .bind_text(4, item.model)
            .bind_text(5, item.lens)
            .bind_optional_text(6, item.capture_time)
            .bind_int64(7, static_cast<std::int64_t>(item.width))
            .bind_int64(8, static_cast<std::int64_t>(item.height))
            .bind_int64(9, static_cast<std::int64_t>(item.orientation))
            .bind_text(10, item.format)
            .bind_text(11, to_string(item.support_status))
            .bind_text(12, item.support_reasons_json)
            .bind_text(13, item.decoder_id)
            .bind_text(14, item.decoder_version)
            .bind_int64(15, detail::to_db(item.probed_at))
            .bind_optional_double(16, item.iso)
            .bind_optional_double(17, item.exposure_time_s)
            .bind_optional_double(18, item.f_number)
            .bind_optional_double(19, item.focal_length_mm);
        if (item.image_count) {
            upsert->bind_int64(20, static_cast<std::int64_t>(*item.image_count));
        } else {
            upsert->bind_null(20);
        }
        Expected<bool> row = upsert->step();
        if (!row) {
            Error error = std::move(row.error());
            if (error.code == ErrorCode::InvalidArgument) {
                error.message =
                    std::format("item of subimage {} rejected: {}", item.subimage_index, error.message);
            }
            return Unexpected(std::move(error));
        }
        if (!*row) {
            return fail(ErrorCode::Internal, "INSERT ... RETURNING returned no row", std::string(where));
        }
        ids.push_back(ItemId{upsert->column_int64(0)});
    }
    if (Status s = savepoint->commit(); !s) {
        return Unexpected(std::move(s.error()));
    }
    return ids;
}

Status WriteTransaction::remove_items(std::span<const ItemId> items) {
    constexpr std::string_view where = "catalog.remove_items";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    Expected<TransactionGuard> savepoint =
        TransactionGuard::begin(*connection_, TransactionGuard::Kind::Savepoint, where);
    if (!savepoint) {
        return Unexpected(std::move(savepoint.error()));
    }
    for (const ItemId id : items) {
        Expected<Statement> remove = connection_->prepare("DELETE FROM items WHERE id = ?1", where);
        if (!remove) {
            return Unexpected(std::move(remove.error()));
        }
        remove->bind_int64(1, id.value);
        if (Status s = remove->run(); !s) {
            return s;
        }
        if (Status s = require_changed(*connection_, "item", id.value, where); !s) {
            return s;
        }
    }
    return savepoint->commit();
}

Status WriteTransaction::set_item_flags(ItemId id, ItemFlags flags) {
    constexpr std::string_view where = "catalog.set_item_flags";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    if (Status valid = flags.validate(); !valid) {
        return valid;
    }
    Expected<Statement> update =
        connection_->prepare("UPDATE items SET pick = ?2, rating = ?3 WHERE id = ?1", where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, id.value).bind_text(2, to_string(flags.pick)).bind_int64(3, flags.rating);
    if (Status s = update->run(); !s) {
        return s;
    }
    return require_changed(*connection_, "item", id.value, where);
}

Expected<ImportId> WriteTransaction::begin_import(const ImportStart& start) {
    constexpr std::string_view where = "catalog.begin_import";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = check_text(start.options_json, "options_json", where); !s) {
        return Unexpected(std::move(s.error()));
    }
    std::string roots = "[";
    for (std::size_t i = 0; i < start.roots.size(); ++i) {
        Expected<std::string> path = path_text(start.roots[i].path, where);
        if (!path) {
            return Unexpected(std::move(path.error()));
        }
        roots +=
            std::format("{}{{\"path\":{},\"include_subfolders\":{}}}", i == 0 ? "" : ",",
                        detail::json_string(*path), start.roots[i].include_subfolders ? "true" : "false");
    }
    roots += "]";
    Expected<Statement> insert = connection_->prepare(
        "INSERT INTO imports (started_at, outcome, roots_json, options_json) VALUES (?1, 'running', ?2, ?3) "
        "RETURNING id",
        where);
    if (!insert) {
        return Unexpected(std::move(insert.error()));
    }
    insert->bind_int64(1, detail::to_db(start.started_at))
        .bind_text(2, roots)
        .bind_text(3, start.options_json);
    Expected<bool> row = insert->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(ErrorCode::Internal, "INSERT ... RETURNING returned no row", std::string(where));
    }
    return ImportId{insert->column_int64(0)};
}

Status WriteTransaction::finish_import(ImportId id, const ImportFinish& finish) {
    constexpr std::string_view where = "catalog.finish_import";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    if (!detail::is_valid(finish.outcome) || finish.outcome == ImportOutcome::Running) {
        return fail(ErrorCode::InvalidArgument, "an import finishes with an outcome other than running",
                    std::string(where));
    }
    const std::uint64_t counts[] = {
        finish.counts.files_seen, finish.counts.added,   finish.counts.refreshed, finish.counts.unchanged,
        finish.counts.duplicates, finish.counts.skipped, finish.counts.failed};
    Expected<Statement> update = connection_->prepare(
        "UPDATE imports SET finished_at = ?2, outcome = ?3, files_seen = ?4, added = ?5, refreshed = ?6, "
        "unchanged = ?7, duplicates = ?8, skipped = ?9, failed = ?10 WHERE id = ?1 AND outcome = 'running'",
        where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, id.value)
        .bind_int64(2, detail::to_db(finish.finished_at))
        .bind_text(3, to_string(finish.outcome));
    for (int i = 0; i < 7; ++i) {
        Expected<std::int64_t> value = count_to_db(counts[i], where);
        if (!value) {
            return Unexpected(std::move(value.error()));
        }
        update->bind_int64(4 + i, *value);
    }
    if (Status s = update->run(); !s) {
        return s;
    }
    if (connection_->changes() > 0) {
        return ok();
    }
    Expected<std::optional<ImportRecord>> record = find_import(id);
    if (!record) {
        return Unexpected(std::move(record.error()));
    }
    if (!record->has_value()) {
        return fail(ErrorCode::NotFound, std::format("no import with id {}", id.value), std::string(where));
    }
    return fail(ErrorCode::InvalidArgument, std::format("import {} has already finished", id.value),
                std::string(where));
}

Expected<std::uint64_t> WriteTransaction::mark_interrupted_imports(Timestamp now) {
    constexpr std::string_view where = "catalog.mark_interrupted_imports";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> update = connection_->prepare(
        "UPDATE imports SET outcome = 'interrupted', finished_at = ?1 WHERE outcome = 'running'", where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, detail::to_db(now));
    if (Status s = update->run(); !s) {
        return Unexpected(std::move(s.error()));
    }
    return static_cast<std::uint64_t>(connection_->changes());
}

// ---------------------------------------------------------------------------------------------
// Recipe revisions
// ---------------------------------------------------------------------------------------------

namespace {

// The item's current revision (std::nullopt: as shot); NotFound when the item does not exist.
Expected<std::optional<RevisionId>> current_of(Connection& connection, ItemId item, std::string_view where) {
    Expected<Statement> statement =
        connection.prepare("SELECT current_revision_id FROM items WHERE id = ?1", where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, item.value);
    Expected<bool> row = statement->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(ErrorCode::NotFound, std::format("no item with id {}", item.value), std::string(where));
    }
    if (statement->is_null(0)) {
        return std::optional<RevisionId>();
    }
    return std::optional<RevisionId>(RevisionId{statement->column_int64(0)});
}

Status move_current(Connection& connection, ItemId item, std::optional<RevisionId> revision,
                    std::string_view where) {
    Expected<Statement> update =
        connection.prepare("UPDATE items SET current_revision_id = ?2 WHERE id = ?1", where);
    if (!update) {
        return Unexpected(std::move(update.error()));
    }
    update->bind_int64(1, item.value);
    if (revision) {
        update->bind_int64(2, revision->value);
    } else {
        update->bind_null(2);
    }
    if (Status s = update->run(); !s) {
        return s;
    }
    return require_changed(connection, "item", item.value, where);
}

}  // namespace

Expected<std::optional<RecipeRevision>> ReadTransaction::find_revision(RevisionId id) const {
    constexpr std::string_view where = "catalog.find_revision";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kRevisionInfoColumns, ", r.settings_json FROM recipe_revisions r WHERE r.id = ?1"}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, id.value);
    return first<RecipeRevision>(
        *statement, RowDecoder(newer_schema_, where),
        [&](const Statement& row, const RowDecoder& decode) { return read_revision(row, decode, where); });
}

Expected<std::optional<RecipeRevision>> ReadTransaction::current_revision(ItemId item) const {
    constexpr std::string_view where = "catalog.current_revision";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::optional<RevisionId>> current = current_of(*connection_, item, where);
    if (!current) {
        return Unexpected(std::move(current.error()));
    }
    if (!current->has_value()) {
        return std::optional<RecipeRevision>();
    }
    Expected<std::optional<RecipeRevision>> revision = find_revision(**current);
    if (revision && !revision->has_value()) {
        // The foreign key makes this impossible in a sound catalog.
        return fail(
            ErrorCode::Corrupt,
            std::format("item {} points at a missing recipe revision {}", item.value, (*current)->value),
            std::string(where));
    }
    return revision;
}

Expected<RevisionHistory> ReadTransaction::revision_history(ItemId item) const {
    constexpr std::string_view where = "catalog.revision_history";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::optional<RevisionId>> current = current_of(*connection_, item, where);
    if (!current) {
        return Unexpected(std::move(current.error()));
    }
    Expected<Statement> statement = connection_->prepare(
        sql({"SELECT ", kRevisionInfoColumns,
             " FROM recipe_revisions r WHERE r.item_id = ?1 ORDER BY r.revision_number"}),
        where);
    if (!statement) {
        return Unexpected(std::move(statement.error()));
    }
    statement->bind_int64(1, item.value);
    Expected<std::vector<RevisionInfo>> revisions =
        collect<RevisionInfo>(*statement, RowDecoder(newer_schema_, where), read_revision_info);
    if (!revisions) {
        return Unexpected(std::move(revisions.error()));
    }
    RevisionHistory history;
    history.item = item;
    history.current = *current;
    history.revisions = std::move(*revisions);
    return history;
}

Expected<RevisionInfo> WriteTransaction::append_revision(ItemId item, const NewRevision& revision) {
    constexpr std::string_view where = "catalog.append_revision";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status valid = revision.validate(); !valid) {
        return Unexpected(std::move(valid.error()));
    }
    Expected<std::optional<RevisionId>> parent = current_of(*connection_, item, where);
    if (!parent) {
        return Unexpected(std::move(parent.error()));
    }
    Expected<std::int64_t> next = [&]() -> Expected<std::int64_t> {
        Expected<Statement> statement = connection_->prepare(
            "SELECT ifnull(max(revision_number), 0) + 1 FROM recipe_revisions WHERE item_id = ?1", where);
        if (!statement) {
            return Unexpected(std::move(statement.error()));
        }
        statement->bind_int64(1, item.value);
        Expected<bool> row = statement->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        return statement->column_int64(0);
    }();
    if (!next) {
        return Unexpected(std::move(next.error()));
    }
    if (*next > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::LimitExceeded, std::format("item {} has too many revisions", item.value),
                    std::string(where));
    }
    Expected<core::Digest256> digest = sha256_of(revision.settings_json);
    if (!digest) {
        return Unexpected(std::move(digest.error()));
    }

    Expected<TransactionGuard> savepoint =
        TransactionGuard::begin(*connection_, TransactionGuard::Kind::Savepoint, where);
    if (!savepoint) {
        return Unexpected(std::move(savepoint.error()));
    }
    Expected<Statement> insert = connection_->prepare(
        "INSERT INTO recipe_revisions (item_id, revision_number, parent_revision_id, created_at, origin, "
        "schema_name, schema_version, settings_json, settings_sha256, engine_version, note) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) RETURNING id",
        where);
    if (!insert) {
        return Unexpected(std::move(insert.error()));
    }
    insert->bind_int64(1, item.value)
        .bind_int64(2, *next)
        .bind_int64(4, detail::to_db(revision.created_at))
        .bind_text(5, to_string(revision.origin))
        .bind_text(6, revision.schema_name)
        .bind_int64(7, static_cast<std::int64_t>(revision.schema_version))
        .bind_text(8, revision.settings_json)
        .bind_blob(9, bytes_of(*digest))
        .bind_text(10, revision.engine_version)
        .bind_text(11, revision.note);
    if (*parent) {
        insert->bind_int64(3, (*parent)->value);
    } else {
        insert->bind_null(3);
    }
    Expected<bool> row = insert->step();
    if (!row) {
        Error error = std::move(row.error());
        if (error.code == ErrorCode::InvalidArgument) {
            error.message = "recipe revision rejected (settings_json must be valid JSON): " + error.message;
        }
        return Unexpected(std::move(error));
    }
    if (!*row) {
        return fail(ErrorCode::Internal, "INSERT ... RETURNING returned no row", std::string(where));
    }
    const RevisionId id{insert->column_int64(0)};
    insert->reset();
    if (Status s = move_current(*connection_, item, id, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    if (Status s = savepoint->commit(); !s) {
        return Unexpected(std::move(s.error()));
    }

    RevisionInfo info;
    info.id = id;
    info.item = item;
    info.revision_number = static_cast<std::uint32_t>(*next);
    info.parent = *parent;
    info.created_at = revision.created_at;
    info.origin = revision.origin;
    info.schema_name = revision.schema_name;
    info.schema_version = revision.schema_version;
    info.settings_sha256 = *digest;
    info.settings_bytes = revision.settings_json.size();
    info.engine_version = revision.engine_version;
    info.note = revision.note;
    return info;
}

Status WriteTransaction::set_current_revision(ItemId item, std::optional<RevisionId> revision) {
    constexpr std::string_view where = "catalog.set_current_revision";
    if (Status s = check_active(connection_, where); !s) {
        return s;
    }
    if (Expected<std::optional<RevisionId>> current = current_of(*connection_, item, where); !current) {
        return Unexpected(std::move(current.error()));
    }
    if (revision) {
        Expected<Statement> owner =
            connection_->prepare("SELECT item_id FROM recipe_revisions WHERE id = ?1", where);
        if (!owner) {
            return Unexpected(std::move(owner.error()));
        }
        owner->bind_int64(1, revision->value);
        Expected<bool> row = owner->step();
        if (!row) {
            return Unexpected(std::move(row.error()));
        }
        if (!*row) {
            return fail(ErrorCode::NotFound, std::format("no recipe revision with id {}", revision->value),
                        std::string(where));
        }
        if (owner->column_int64(0) != item.value) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("recipe revision {} belongs to item {}, not to item {}", revision->value,
                                    owner->column_int64(0), item.value),
                        std::string(where));
        }
    }
    return move_current(*connection_, item, revision, where);
}

Expected<std::optional<RevisionId>> WriteTransaction::undo_revision(ItemId item) {
    constexpr std::string_view where = "catalog.undo_revision";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::optional<RevisionId>> current = current_of(*connection_, item, where);
    if (!current) {
        return Unexpected(std::move(current.error()));
    }
    if (!current->has_value()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("item {} shows its as-shot settings: nothing to undo", item.value),
                    "catalog.undo_revision.nothing_to_undo");
    }
    Expected<Statement> parent =
        connection_->prepare("SELECT parent_revision_id FROM recipe_revisions WHERE id = ?1", where);
    if (!parent) {
        return Unexpected(std::move(parent.error()));
    }
    parent->bind_int64(1, (*current)->value);
    Expected<bool> row = parent->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(
            ErrorCode::Corrupt,
            std::format("item {} points at a missing recipe revision {}", item.value, (*current)->value),
            std::string(where));
    }
    const std::optional<RevisionId> target =
        parent->is_null(0) ? std::nullopt : std::optional<RevisionId>(RevisionId{parent->column_int64(0)});
    parent->reset();
    if (Status s = move_current(*connection_, item, target, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    return target;
}

Expected<RevisionId> WriteTransaction::redo_revision(ItemId item) {
    constexpr std::string_view where = "catalog.redo_revision";
    if (Status s = check_active(connection_, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    Expected<std::optional<RevisionId>> current = current_of(*connection_, item, where);
    if (!current) {
        return Unexpected(std::move(current.error()));
    }
    // "IS ?2" matches NULL too: the newest revision derived from as shot.
    Expected<Statement> child = connection_->prepare(
        "SELECT id FROM recipe_revisions WHERE item_id = ?1 AND parent_revision_id IS ?2 "
        "ORDER BY revision_number DESC LIMIT 1",
        where);
    if (!child) {
        return Unexpected(std::move(child.error()));
    }
    child->bind_int64(1, item.value);
    if (*current) {
        child->bind_int64(2, (*current)->value);
    } else {
        child->bind_null(2);
    }
    Expected<bool> row = child->step();
    if (!row) {
        return Unexpected(std::move(row.error()));
    }
    if (!*row) {
        return fail(
            ErrorCode::InvalidArgument,
            std::format("item {} has no revision derived from its current one: nothing to redo", item.value),
            "catalog.redo_revision.nothing_to_redo");
    }
    const RevisionId target{child->column_int64(0)};
    child->reset();
    if (Status s = move_current(*connection_, item, target, where); !s) {
        return Unexpected(std::move(s.error()));
    }
    return target;
}

}  // namespace lucent::catalog
