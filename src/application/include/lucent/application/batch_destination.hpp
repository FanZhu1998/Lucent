#pragma once

// Where a batch writes its outputs and what they are called (blueprint section 2, "Automatic batch
// development", step 3; section 7, "Export integrity and user-selected destinations": "Use names
// such as {original-stem}_Lucent_{revision} with optional subfolders mirroring the input. Validate
// Windows names, long/Unicode paths, and case-insensitive collisions. Default to creating a unique
// new filename; never overwrite an original.").
//
// Name patterns. A pattern is UTF-8 text with tokens in braces:
//   {original-stem}  the source file name without its last extension ("IMG_0001" for IMG_0001.CR3;
//                    "image" when it is empty)
//   {revision}       the number of the recipe revision the output renders; 0 for as-shot settings
//   {image}          the image of a multi-image file, counted from 1
//   {sequence}       the photograph's position in the batch, counted from 1, at least four digits
// A pattern must contain {original-stem} or {sequence} (otherwise every output would compete for
// one name), may not contain a path separator or a character Windows forbids in file names
// (< > : " / \ | ? * and control characters), and is at most kMaxNamePatternBytes long. Literal
// "{" and "}" are not allowed. An output of a multi-image file whose pattern has no {image} gets
// "_img{k}" appended (the W3 rule), so the images of one file never compete for a name.
//
// Expansion (expand_output_name) produces a valid Windows file name: trailing dots and spaces are
// removed (Windows drops them silently, which would make two names one), a reserved device name
// (CON, PRN, AUX, NUL, COM0-9, LPT0-9 and their superscript-digit forms, CONIN$, CONOUT$, before
// the first dot) is prefixed with "_", and the name part is shortened (never splitting a surrogate
// pair) so that the whole name, with "_{n}" and the extension, fits the 255 UTF-16 unit component
// limit. Uniqueness index n = 1 adds nothing; n >= 2 appends "_{n}".
//
// Mirrored subfolders. With BatchDestination::mirror_subfolders, an output goes to
// folder / (the source's folder relative to the mirror root). Each mirrored component is made a
// valid Windows name the same way. A source outside the mirror root goes to the folder itself.
//
// Paths are compared with the file system's case rules (detail::fold_case), so "img_0001.JPG"
// collides with "IMG_0001.jpg" even in a case-sensitive directory. Long paths (above MAX_PATH)
// are supported throughout; a final path longer than kMaxOutputPathChars is refused.
//
// Thread-safety: plain value types and pure functions (directory listings aside).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/export/export.hpp>

namespace lucent::app {

inline constexpr std::string_view kDefaultNamePattern = "{original-stem}_Lucent_{revision}";
// The single-item export's names of W3 ("IMG_0001_Lucent.jpg", then "_2", "_3", ...).
inline constexpr std::string_view kSingleExportNamePattern = "{original-stem}_Lucent";
inline constexpr std::size_t kMaxNamePatternBytes = 200;
inline constexpr std::size_t kMaxFileNameUnits = 255;   // UTF-16 code units of one component
inline constexpr std::size_t kMaxOutputPathChars = 32000;  // below the 32,767 unit limit of "\\?\" paths
inline constexpr std::uint32_t kMaxOutputNameIndex = 9999;

// What happens when an output's name is already taken in the destination. Persisted in batch
// destinations ("unique_name", "overwrite"); append only.
enum class CollisionPolicy : std::uint8_t {
    // Default: the first free name with "_2", "_3", ... appended. Nothing is ever replaced.
    UniqueName = 1,
    // Replace an existing file of that name, revalidating its identity at publication. Never a
    // library original (a file any library item renders) and never another output of the same
    // batch: those still get a unique new name, with a review note.
    Overwrite = 2,
};

[[nodiscard]] std::string_view to_string(CollisionPolicy policy) noexcept;
// InvalidArgument ("app.batch.destination") for an unknown spelling.
[[nodiscard]] Expected<CollisionPolicy> collision_policy_from_string(std::string_view text);

struct BatchDestination {
    // The output folder (absolute). Created, with missing parents, when it does not exist yet.
    std::filesystem::path folder;
    std::string name_pattern{kDefaultNamePattern};
    bool mirror_subfolders = false;
    // With mirror_subfolders: the input folder whose structure is mirrored. std::nullopt: the
    // folder selected for a folder batch, otherwise the deepest folder containing every source.
    std::optional<std::filesystem::path> mirror_root;
    CollisionPolicy collisions = CollisionPolicy::UniqueName;
};

enum class NameToken : std::uint8_t {
    Literal = 0,
    OriginalStem = 1,
    Revision = 2,
    Image = 3,
    Sequence = 4,
};

struct NamePatternPart {
    NameToken token = NameToken::Literal;
    std::wstring literal;  // Literal only
};

struct NamePattern {
    std::string text;  // as given (UTF-8)
    std::vector<NamePatternPart> parts;
    bool has_image = false;
};

// Parses and validates a pattern (see the file comment). InvalidArgument ("app.batch.name") with
// a developer-facing reason for an empty, too long, unbalanced or invalid pattern, an unknown
// token, a forbidden character or no {original-stem} / {sequence}.
[[nodiscard]] Expected<NamePattern> parse_name_pattern(std::string_view pattern);

// The facts a name is made from.
struct OutputNameFields {
    std::filesystem::path source;      // the source file (only its stem is used)
    std::uint32_t revision_number = 0;  // 0: as shot
    std::uint32_t subimage = 0;
    std::uint32_t subimage_count = 1;
    std::uint32_t ordinal = 0;  // position in the batch, from 0 ({sequence} is ordinal + 1)
};

// ".jpg" / ".tif" for the format (export_service.hpp's export_extension).
// The file name (no directory) for `fields` with uniqueness index `index` (1: no suffix).
// InvalidArgument ("app.batch.name") for index 0, an unknown format, subimage >= subimage_count
// or a source without a file name; LimitExceeded above kMaxOutputNameIndex.
[[nodiscard]] Expected<std::filesystem::path> expand_output_name(const NamePattern& pattern,
                                                                 const OutputNameFields& fields,
                                                                 exporter::ExportFormat format,
                                                                 std::uint32_t index);

// `name` made a valid Windows file-name component (see the file comment) with at most
// `max_units` UTF-16 units: forbidden characters become "_", trailing dots and spaces are
// removed, a reserved device name gets a "_" prefix, an empty result becomes "_". Unpaired
// surrogates become U+FFFD.
[[nodiscard]] std::wstring sanitize_file_name(std::wstring_view name, std::size_t max_units = kMaxFileNameUnits);

// ok() when `name` is a valid Windows file-name component as it stands; InvalidArgument
// ("app.batch.name") naming the problem otherwise.
[[nodiscard]] Status validate_file_name(std::wstring_view name);

// True for CON, PRN, AUX, NUL, COM0-9, LPT0-9 (and the superscript digits 1-3), CONIN$ and
// CONOUT$, compared case-insensitively on the part before the first dot, trailing spaces ignored.
[[nodiscard]] bool is_reserved_device_name(std::wstring_view name) noexcept;

// The folder an output of `source` goes to (see "Mirrored subfolders"). `folder` and
// `mirror_root` should be canonical. Never fails: invalid components are sanitised.
[[nodiscard]] std::filesystem::path output_directory_for(const std::filesystem::path& folder, bool mirror_subfolders,
                                                         const std::filesystem::path& mirror_root,
                                                         const std::filesystem::path& source);

// The deepest folder that contains every path in `sources` (compared case-insensitively);
// empty when they share none (different drives) or `sources` is empty.
[[nodiscard]] std::filesystem::path common_parent_folder(const std::vector<std::filesystem::path>& sources);

// The text a name is compared by: the full path folded with the file system's case rules.
[[nodiscard]] std::wstring folded_path_key(const std::filesystem::path& path);

}  // namespace lucent::app
