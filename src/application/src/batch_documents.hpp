#pragma once

// Private: the application-owned JSON documents of a batch in the catalog's job journal
// (catalog/jobs.hpp stores them as opaque text; ADR 0012, "Documents").
//
//   destination_json      "lucent.batch-destination" v1: folder (and the identity of the folder
//                         object when the batch was created), name pattern, mirrored subfolders and
//                         their root, collision policy, the batch's temporary-name tag, whether
//                         report files are written, the storage kind.
//   export_settings_json  "lucent.batch-export" v1: everything that determines an output's bytes
//                         besides the pixels: format, profile, intent, black-point compensation, JPEG
//                         quality, TIFF compression, metadata policy, rights text, and whether
//                         unsupported photographs are exported. Written deterministically (sorted
//                         members, no whitespace), because its SHA-256 is part of every job identity.
//   policy_json           "lucent.batch-policy" v1: existing-output policy, retry policy (with
//                         max_interruptions), whether the batch is an interactive single export,
//                         the photographs excluded at submission with their reasons (for the
//                         report), and the automatic settings' related-shot options (whether related
//                         shots are kept consistent, the user's groups and anchors by job ordinal;
//                         the batch's automatic mode itself is the catalog's batches.auto_mode).
//
// Readers accept exactly their version: an older or unknown document is Corrupt, a newer one is
// VersionMismatch ("app.batch.document"). Members added to a version after its first release
// (policy: retry.max_interruptions, single_export, automatic) are optional on reading and take their
// defaults.
// Paths are UTF-8.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/batch_destination.hpp>
#include <lucent/application/batch_service.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/storage.hpp>
#include <lucent/export/export.hpp>

namespace lucent::app::detail {

struct DestinationDocument {
    std::filesystem::path folder;  // canonical
    // Volume serial and file id of the folder when the batch was created (size and time unused).
    std::optional<core::FileIdentity> folder_identity;
    std::string name_pattern;
    bool mirror_subfolders = false;
    std::filesystem::path mirror_root;  // empty: none
    CollisionPolicy collisions = CollisionPolicy::UniqueName;
    std::wstring temp_tag;  // 8 lowercase hex digits
    bool write_report_files = true;
    core::StorageKind storage = core::StorageKind::Unknown;
};

struct ExportDocument {
    exporter::ExportSettings settings;
    std::optional<std::string> artist;
    std::optional<std::string> copyright;
    bool allow_unsupported = false;
};

// Related-shot options of an automatic batch (BatchRequest::keep_consistent, BatchItem::group and
// anchor), by job ordinal.
struct AutomaticPolicy {
    bool keep_consistent = false;
    std::map<std::uint32_t, std::string> groups;
    std::set<std::uint32_t> anchors;
};

struct PolicyDocument {
    ExistingOutputs existing_outputs = ExistingOutputs::Skip;
    BatchRetryPolicy retry;
    bool single_export = false;  // BatchRequest::single_export
    std::vector<PlanExclusion> excluded;  // at most kMaxListedExclusions
    std::uint64_t excluded_count = 0;
    AutomaticPolicy automatic;
};

[[nodiscard]] Expected<std::string> write_destination(const DestinationDocument& document);
[[nodiscard]] Expected<DestinationDocument> read_destination(std::string_view json);

[[nodiscard]] Expected<std::string> write_export_document(const ExportDocument& document);
[[nodiscard]] Expected<ExportDocument> read_export_document(std::string_view json);

[[nodiscard]] Expected<std::string> write_policy(const PolicyDocument& document);
[[nodiscard]] Expected<PolicyDocument> read_policy(std::string_view json);

// "<16 hex volume serial>:<32 hex file id>" of an identity's file object, and back.
[[nodiscard]] std::string file_object_text(const core::FileIdentity& identity);

}  // namespace lucent::app::detail
