#pragma once

// lucent-auto's documents (developer tool; see auto_tool.hpp): the per-item and run JSON, the
// summary table, the evaluation sheets and the side-by-side previews.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <lucent/analysis/related_shots.hpp>
#include <lucent/export/encode.hpp>

#include "auto_tool.hpp"

namespace lucent::auto_tool {

using Json = nlohmann::ordered_json;

inline constexpr const char* kRunSchema = "lucent.auto-run";
inline constexpr const char* kItemSchema = "lucent.auto-item";
inline constexpr const char* kBlindKeySchema = "lucent.blind-key";
inline constexpr const char* kBlindChoicesSchema = "lucent.blind-choices";
inline constexpr int kSchemaVersion = 1;

// One analysed (or failed) item as the reports see it.
struct ItemReport {
    std::size_t index = 0;
    InputFile input;
    std::optional<std::uint32_t> subimage;
    std::uint32_t subimage_count = 1;
    const analysis::RelatedItemResult* result = nullptr;
    std::string group_id;
    analysis::GroupBasis group_basis = analysis::GroupBasis::Single;
    std::size_t group_size = 1;
    std::string decision;
    // Relative to the output directory ('/' separators); empty when not written.
    std::string item_json;
    std::string as_shot_jpg;
    std::string auto_jpg;
    std::string pair_jpg;
    std::optional<Error> preview_error;
    // No visible change: the automatic settings equal as shot, or the two 8-bit previews differ by
    // at most kUnchangedCodeValues anywhere. Such pairs are left out of the blind sheet (they would
    // only dilute a preference count) and listed as unchanged in the key.
    bool unchanged = false;
};

inline constexpr int kUnchangedCodeValues = 1;

// True when the two 8-bit images have the same size and no sample differs by more than
// kUnchangedCodeValues.
[[nodiscard]] bool visibly_identical(const exporter::OutputImage& a, const exporter::OutputImage& b);

// "<NNN>-<name>" of an item.
[[nodiscard]] std::string item_stem(std::size_t index, const std::string& name);

[[nodiscard]] Json item_json(const ItemReport& item);
[[nodiscard]] Json run_json(const Arguments& args, const Discovery& discovery,
                            const analysis::RelatedResult& result, std::span<const ItemReport> items,
                            double seconds);
[[nodiscard]] std::string summary_tsv(std::span<const ItemReport> items);
// The fixed-width table printed on standard output.
[[nodiscard]] std::string summary_table(std::span<const ItemReport> items);

// The sheets; `jpeg(path)` returns the bytes of a preview file (relative path), empty when missing.
using JpegReader = std::vector<std::uint8_t> (*)(const std::filesystem::path& out_dir, const std::string& relative);
[[nodiscard]] std::string sheet_html(const Arguments& args, const analysis::RelatedResult& result,
                                     std::span<const ItemReport> items, const std::filesystem::path& out_dir,
                                     JpegReader jpeg);
[[nodiscard]] std::string blind_html(const Arguments& args, std::span<const ItemReport> items,
                                     const std::filesystem::path& out_dir, JpegReader jpeg);
[[nodiscard]] Json blind_key(const Arguments& args, std::span<const ItemReport> items);

[[nodiscard]] std::string base64(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string html_escape(std::string_view text);

// `left` and `right` side by side on a dark background with a 16-pixel gap (8-bit sRGB).
[[nodiscard]] exporter::OutputImage side_by_side(const exporter::OutputImage& left, const exporter::OutputImage& right);

}  // namespace lucent::auto_tool
