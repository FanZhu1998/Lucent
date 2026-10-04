#pragma once

// Recipes: the persisted form of engine::DevelopSettings (blueprint section 8, "Recipes, assets,
// and forward compatibility"; ADR 0010).
//
// Document. A recipe is a JSON object of the shared contract "lucent.develop", version 1:
//
//   {"base_look": {"id": "neutral", "version": 1},
//    "contrast": 0, "exposure_ev": 0.35, "highlights": 0,
//    "locks": ["exposure"],
//    "operations": {"color": 1, "contrast": 1, "exposure": 1, "local_tone": 1, "tone_curve": 1,
//                   "white_balance": 1},
//    "saturation": 0, "schema": "lucent.develop", "shadows": 0,
//    "tone_curve": [[0, 0], [1, 1]],
//    "version": 1, "vibrance": 0,
//    "white_balance": {"mode": "as_shot"}}
//
// (shown wrapped; the canonical text has no whitespace). White balance is one of
//   {"mode": "as_shot"}
//   {"mode": "kelvin", "temperature_k": <number>, "tint": <number>}
//   {"mode": "camera_neutral", "r": <number>, "g": <number>, "b": <number>}
// "locks" lists DevelopControl names (engine::to_string) in their numeric order; "tone_curve" is
// the list of [x, y] control points; "operations" records the semantic version of every
// operation the settings were made for. Semantics and ranges are DevelopSettings v1's
// (develop_settings.hpp).
//
// Canonical form (write_recipe). The JSON Canonicalization Scheme, RFC 8785: no whitespace,
// object members sorted by their names' UTF-16 code units (all names are ASCII here), strings with
// only the mandatory escapes, and numbers formatted as ECMAScript's Number.prototype.toString
// does (the shortest decimal that reads back as the same IEEE double, plain notation for
// 1e-6 <= |x| < 1e21, otherwise "d.ddde+NN"; -0 is written "0"). Every field is written, so
// equal settings always give byte-identical text, and the recipe identity, settings_sha256 =
// SHA-256 of that UTF-8 text, is reproducible by any JCS implementation. Doubles survive a write
// and read exactly (the shortest round-trip representation).
//
// Strict reading (read_recipe). Errors use "app.recipe":
//   * not JSON, not an object, a repeated member name, "schema" other than "lucent.develop"
//     -> InvalidArgument;
//   * "version" newer than kRecipeSchemaVersion -> VersionMismatch (nothing else is read: a newer
//     document is never interpreted); version 0 or not an unsigned integer -> InvalidArgument;
//   * in a version-1 document: an unknown member at any level, an unknown operation in
//     "operations", an unknown white-balance mode or lock name, a missing member, a member of the
//     wrong JSON type, or a value DevelopSettings::validate() rejects -> InvalidArgument;
//   * an operation or base-look version other than the one this build implements, or a base look
//     this build does not know -> VersionMismatch (made by another Lucent: never treated as a
//     no-op; the item is then opened read-only, recipe_service.hpp).
// Numbers may be written in any JSON form (1, 1.0, 1e0); integers (versions) must be unsigned
// integers without a fraction or exponent.
//
// Paste categories (copy/paste by category, recipe_service.hpp):
//   WhiteBalance  white_balance
//   Exposure      exposure_ev
//   Tone          contrast, tone_curve, base_look
//   Color         saturation, vibrance
//   Local         highlights, shadows (the guided-filter local tone operation; masks join it in a
//                 later schema)
// Locks travel with their controls.
//
// Thread-safety: pure functions; reentrant.

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/core/error.hpp>
#include <lucent/core/hash.hpp>
#include <lucent/engine/develop_settings.hpp>

namespace lucent::app {

// The recipe contract: the engine's DevelopSettings schema and version.
inline constexpr std::string_view kRecipeSchema = engine::kDevelopSettingsSchema;
inline constexpr std::uint32_t kRecipeSchemaVersion = engine::kDevelopSettingsVersion;

// A recipe as stored: its canonical text and the SHA-256 of that text (the recipe identity).
struct RecipeDocument {
    std::string json;
    core::Digest256 sha256{};
};

// The canonical document of `settings` (see the file comment). Errors: those of
// settings.validate() (only valid settings are written); OutOfMemory.
[[nodiscard]] Expected<RecipeDocument> write_recipe(const engine::DevelopSettings& settings);

// Reads a "lucent.develop" document strictly (see the file comment).
[[nodiscard]] Expected<engine::DevelopSettings> read_recipe(std::string_view json);

// RFC 8785 number formatting of a finite double (see the file comment); "0" for -0.
// InvalidArgument for NaN and infinities (JSON has no form for them).
[[nodiscard]] Expected<std::string> canonical_number(double value);

// ---- Categories ------------------------------------------------------------------------------

// Persisted in clipboards and reports as to_string(); append only.
enum class RecipeCategory : std::uint8_t {
    WhiteBalance = 1,
    Exposure = 2,
    Tone = 3,
    Color = 4,
    Local = 5,
};

inline constexpr std::array<RecipeCategory, 5> kAllRecipeCategories = {
    RecipeCategory::WhiteBalance, RecipeCategory::Exposure, RecipeCategory::Tone, RecipeCategory::Color,
    RecipeCategory::Local};

// "white_balance", "exposure", "tone", "color", "local"; "invalid" for other values.
[[nodiscard]] std::string_view to_string(RecipeCategory category) noexcept;
[[nodiscard]] std::optional<RecipeCategory> parse_recipe_category(std::string_view text) noexcept;

// The category a control belongs to (see the file comment).
[[nodiscard]] RecipeCategory category_of(engine::DevelopControl control) noexcept;

// The controls of `category`, in DevelopControl order.
[[nodiscard]] std::vector<engine::DevelopControl> controls_of(RecipeCategory category);

// Every DevelopControl, in numeric order.
inline constexpr std::array<engine::DevelopControl, 9> kAllDevelopControls = {
    engine::DevelopControl::WhiteBalance, engine::DevelopControl::Exposure, engine::DevelopControl::Contrast,
    engine::DevelopControl::Highlights,   engine::DevelopControl::Shadows,  engine::DevelopControl::ToneCurve,
    engine::DevelopControl::Saturation,   engine::DevelopControl::Vibrance, engine::DevelopControl::BaseLook};

// True when `a` and `b` have the same value for `control` (exact comparison).
[[nodiscard]] bool same_value(const engine::DevelopSettings& a, const engine::DevelopSettings& b,
                              engine::DevelopControl control) noexcept;

// Copies the value of `control` from `from` to `to`.
void copy_value(const engine::DevelopSettings& from, engine::DevelopSettings& to,
                engine::DevelopControl control);

// Controls whose values differ between `a` and `b`, in DevelopControl order (locks are not
// values; see changed_locks()).
[[nodiscard]] std::vector<engine::DevelopControl> changed_controls(const engine::DevelopSettings& a,
                                                                   const engine::DevelopSettings& b);

// Controls locked in exactly one of `a` and `b`, in DevelopControl order.
[[nodiscard]] std::vector<engine::DevelopControl> changed_locks(const engine::DevelopSettings& a,
                                                                const engine::DevelopSettings& b);

}  // namespace lucent::app
