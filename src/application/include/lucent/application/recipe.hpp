#pragma once

// Recipes: the persisted form of engine::DevelopSettings (blueprint section 8, "Recipes, assets,
// and forward compatibility"; ADR 0010, ADR 0015, ADR 0023).
//
// Document. A recipe is a JSON object of the shared contract "lucent.develop", version 3:
//
//   {"base_look": {"id": "neutral", "version": 1},
//    "chroma_denoise": {"radius": 8, "strength": 0},
//    "contrast": 0, "exposure_ev": 0.35,
//    "geometry": {"crop": {"bottom": 1, "left": 0, "right": 1, "top": 0},
//                 "flip_horizontal": false, "quarter_turns": 0, "straighten_degrees": 0},
//    "highlights": 0,
//    "lens_correction": {"distortion": true, "lateral_chromatic_aberration": true,
//                        "profile": null, "vignetting": true},
//    "local_adjustments": [<adjustment>, ...],
//    "locks": ["exposure"],
//    "operations": {"chroma_denoise": 1, "color": 1, "contrast": 1, "exposure": 1, "geometry": 2,
//                   "lens_correction": 1, "local_adjustments": 1, "local_tone": 1, "tone_curve": 1,
//                   "white_balance": 1},
//    "saturation": 0, "schema": "lucent.develop", "shadows": 0,
//    "tone_curve": [[0, 0], [1, 1]],
//    "version": 3, "vibrance": 0,
//    "white_balance": {"mode": "as_shot"}}
//
// (shown wrapped; the canonical text has no whitespace). White balance is one of
//   {"mode": "as_shot"}
//   {"mode": "kelvin", "temperature_k": <number>, "tint": <number>}
//   {"mode": "camera_neutral", "r": <number>, "g": <number>, "b": <number>}
// "locks" lists DevelopControl names (engine::to_string) in their numeric order; "tone_curve" is
// the list of [x, y] control points; "operations" records the semantic version of every
// operation the settings were made for. A local adjustment (engine::LocalAdjustment, in list
// order) is
//   {"controls": {"contrast": <n>, "exposure_ev": <n>, "highlights": <n>, "saturation": <n>,
//                 "shadows": <n>, "temperature": <n>, "tint": <n>},
//    "enabled": <bool>, "id": <unsigned>, "invert": <bool>, "mask": <mask>, "opacity": <n>}
// and its mask one of (points are [x, y] in the normalised upright frame, develop_settings.hpp)
//   {"kind": "brush", "strokes": [{"flow": <n>, "hardness": <n>, "mode": "add" | "erase",
//                                  "points": [[x, y], ...], "radius": <n>}, ...]}
//   {"kind": "linear", "end": [x, y], "feather": <n>, "start": [x, y]}
//   {"kind": "radial", "angle_degrees": <n>, "center": [x, y], "feather": <n>,
//    "radius_x": <n>, "radius_y": <n>}
// "lens_correction" (engine::LensCorrectionSetting, lens_correction.hpp) holds the three enable
// flags and "profile": null for the automatic profile (the lens database's exact match only) or a
// named Lensfun profile {"crop_factor": <n>, "maker": <string>, "model": <string>}. Its operation
// "lens_correction" is 1, or 0 ("none") for settings made before Lucent had lens corrections: they
// correct nothing and their member holds the default. Semantics and ranges are DevelopSettings v3's
// (develop_settings.hpp, ADR 0015, ADR 0023).
//
// Older versions are still read and converted with engine::upgrade_develop_settings(), so
// read_recipe() always returns version-3 settings, every older document renders exactly as before
// and equal settings have one identity whichever version they were stored in. Only version 3 is
// written:
//   * Version 2 (ADR 0015) is version 3 without "lens_correction" and its operation; it converts to
//     lens-correction operation version 0 (no lens correction).
//   * Version 1 (ADR 0010) is version 2 without "chroma_denoise", "geometry", "local_adjustments"
//     and their three operations; it converts to no denoising, no masks, identity geometry of
//     version 1 and lens-correction operation version 0.
//
// Canonical form (write_recipe). The JSON Canonicalization Scheme, RFC 8785: no whitespace,
// object members sorted by their names' UTF-16 code units (all names are ASCII here), strings with
// only the mandatory escapes, and numbers formatted as ECMAScript's Number.prototype.toString
// does (the shortest decimal that reads back as the same IEEE double, plain notation for
// 1e-6 <= |x| < 1e21, otherwise "d.ddde+NN"; -0 is written "0"). Every member is written (also
// the defaults of the version-2 and version-3 members, and "profile": null), so equal settings
// always give byte-identical text, and
// the recipe identity, settings_sha256 = SHA-256 of that UTF-8 text, is reproducible by any JCS
// implementation. Doubles survive a write and read exactly (the shortest round-trip
// representation). Arrays keep their order (masks, strokes and points are ordered values).
//
// Strict reading (read_recipe). Errors use "app.recipe":
//   * not JSON, not an object, a repeated member name, "schema" other than "lucent.develop"
//     -> InvalidArgument;
//   * "version" newer than kRecipeSchemaVersion -> VersionMismatch (nothing else is read: a newer
//     document is never interpreted); version 0 or not an unsigned integer -> InvalidArgument;
//   * in a version-1, version-2 or version-3 document: an unknown member at any level (in an older
//     document also a member of a later version), a missing member (a version-2 document without
//     the version-2 members, a version-3 document without "lens_correction" or one of its members),
//     an unknown operation in "operations" (in an older document also one of a later version), an
//     unknown white-balance mode, lock name, mask kind or brush mode, a member of the wrong JSON type
//     (an id or quarter_turns that is not an unsigned integer, a flag that is not a boolean, a
//     profile that is neither null nor an object, a profile name that is not a string), or a value
//     DevelopSettings::validate() rejects (a lens-correction operation version 0 with a member
//     other than the default, a profile name or crop factor out of its bounds) -> InvalidArgument;
//   * an operation or base-look version other than the one this build implements (the geometry:
//     1 or 2; the lens correction: 0 or 1), or a base look this build does not know ->
//     VersionMismatch (made by another Lucent: never treated as a no-op; the item is then opened
//     read-only, recipe_service.hpp).
// Numbers may be written in any JSON form (1, 1.0, 1e0); integers (versions, ids, quarter turns)
// must be unsigned integers without a fraction or exponent.
//
// Paste categories (copy/paste by category, recipe_service.hpp):
//   WhiteBalance  white_balance
//   Exposure      exposure_ev
//   Tone          contrast, tone_curve, base_look
//   Color         saturation, vibrance
//   Local         highlights, shadows (the guided-filter local tone operation)
//   Masks         the masks (local_adjustments, RecipePart::LocalAdjustments); not part of
//                 kDefaultRecipeCategories (masks are placed on one photo's content, and pasting
//                 them replaces the target's masks, also by an empty list: only when chosen)
//   Detail        chroma_denoise (RecipePart::ChromaDenoise)
//   Geometry      straighten, crop, quarter turns and flip (RecipePart::Geometry); not part of
//                 kDefaultRecipeCategories (a crop rarely belongs on another photo)
//   LensCorrections  lens_correction with its operation version (RecipePart::LensCorrection);
//                 not part of kDefaultRecipeCategories: a lens profile and its corrections belong to
//                 one camera and lens, so the recipe service pastes them only between photos of the
//                 same camera model and lens model (recipe_service.hpp)
// Locks travel with their controls. The parts (RecipePart) have no lock: the automatic settings
// never change them (they adjust global controls only, auto_develop.hpp), so there is nothing to
// protect them from.
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
// The oldest document version read (converted on read, see the file comment).
inline constexpr std::uint32_t kOldestRecipeSchemaVersion = engine::kOldestDevelopSettingsVersion;

// The canonical text of the as-shot settings as recipe version 1 wrote them (W6 to W8, ADR 0010;
// identity 671dbaa8...). Batches frozen by those builds name its SHA-256 for photos exported as shot;
// this build reads it (geometry version 1) and renders it exactly as then (ADR 0022).
inline constexpr std::string_view kVersion1AsShotRecipe =
    R"({"base_look":{"id":"neutral","version":1},"contrast":0,"exposure_ev":0,"highlights":0,"locks":[],)"
    R"("operations":{"color":1,"contrast":1,"exposure":1,"local_tone":1,"tone_curve":1,"white_balance":1},)"
    R"("saturation":0,"schema":"lucent.develop","shadows":0,"tone_curve":[[0,0],[1,1]],"version":1,)"
    R"("vibrance":0,"white_balance":{"mode":"as_shot"}})";

// The canonical texts of the as-shot settings as recipe version 2 wrote them: with geometry version 2
// (W10 to W12, ADR 0017; identity 879de8b7...) and geometry version 1 (W9, ADR 0015; identity
// 3104dbcc...). Batches frozen by those builds name their SHA-256 for photos exported as shot; this
// build reads them (lens-correction operation version 0: no lens correction) and renders them exactly
// as then (ADR 0023).
inline constexpr std::array<std::string_view, 2> kVersion2AsShotRecipes = {
    R"({"base_look":{"id":"neutral","version":1},"chroma_denoise":{"radius":8,"strength":0},"contrast":0,)"
    R"("exposure_ev":0,"geometry":{"crop":{"bottom":1,"left":0,"right":1,"top":0},"flip_horizontal":false,)"
    R"("quarter_turns":0,"straighten_degrees":0},"highlights":0,"local_adjustments":[],"locks":[],)"
    R"("operations":{"chroma_denoise":1,"color":1,"contrast":1,"exposure":1,"geometry":2,"local_adjustments":1,)"
    R"("local_tone":1,"tone_curve":1,"white_balance":1},"saturation":0,"schema":"lucent.develop","shadows":0,)"
    R"("tone_curve":[[0,0],[1,1]],"version":2,"vibrance":0,"white_balance":{"mode":"as_shot"}})",
    R"({"base_look":{"id":"neutral","version":1},"chroma_denoise":{"radius":8,"strength":0},"contrast":0,)"
    R"("exposure_ev":0,"geometry":{"crop":{"bottom":1,"left":0,"right":1,"top":0},"flip_horizontal":false,)"
    R"("quarter_turns":0,"straighten_degrees":0},"highlights":0,"local_adjustments":[],"locks":[],)"
    R"("operations":{"chroma_denoise":1,"color":1,"contrast":1,"exposure":1,"geometry":1,"local_adjustments":1,)"
    R"("local_tone":1,"tone_curve":1,"white_balance":1},"saturation":0,"schema":"lucent.develop","shadows":0,)"
    R"("tone_curve":[[0,0],[1,1]],"version":2,"vibrance":0,"white_balance":{"mode":"as_shot"}})"};

// A recipe as stored: its canonical text and the SHA-256 of that text (the recipe identity).
struct RecipeDocument {
    std::string json;
    core::Digest256 sha256{};
};

// The canonical version-3 document of `settings` (see the file comment); older settings are
// upgraded first (engine::upgrade_develop_settings), so they get the identity of their conversion.
// Errors: those of settings.validate() (only valid settings are written); OutOfMemory.
[[nodiscard]] Expected<RecipeDocument> write_recipe(const engine::DevelopSettings& settings);

// Reads a "lucent.develop" document strictly (see the file comment). The settings are always
// version 3 (an older document is upgraded).
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
    Local = 5,            // highlights and shadows
    Detail = 6,           // chroma denoising (version 2)
    Geometry = 7,         // straighten, crop, quarter turns, flip (version 2)
    Masks = 8,            // the masks (version 2)
    LensCorrections = 9,  // the lens-profile corrections (version 3)
};

inline constexpr std::array<RecipeCategory, 9> kAllRecipeCategories = {
    RecipeCategory::WhiteBalance, RecipeCategory::Exposure, RecipeCategory::Tone,
    RecipeCategory::Color,        RecipeCategory::Local,    RecipeCategory::Masks,
    RecipeCategory::Detail,       RecipeCategory::Geometry, RecipeCategory::LensCorrections};

// What copy/paste offers selected by default: every category but Masks and Geometry (placed on the
// photo's content) and LensCorrections (one camera's and lens's).
inline constexpr std::array<RecipeCategory, 6> kDefaultRecipeCategories = {
    RecipeCategory::WhiteBalance, RecipeCategory::Exposure, RecipeCategory::Tone,
    RecipeCategory::Color,        RecipeCategory::Local,    RecipeCategory::Detail};

// "white_balance", "exposure", "tone", "color", "local", "masks", "detail", "geometry",
// "lens_corrections"; "invalid" for other values.
[[nodiscard]] std::string_view to_string(RecipeCategory category) noexcept;
[[nodiscard]] std::optional<RecipeCategory> parse_recipe_category(std::string_view text) noexcept;

// The category a control belongs to (see the file comment).
[[nodiscard]] RecipeCategory category_of(engine::DevelopControl control) noexcept;

// The controls of `category`, in DevelopControl order (none for Masks, Detail, Geometry and
// LensCorrections).
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

// ---- Parts (versions 2 and 3) ------------------------------------------------------------------

// The members of DevelopSettings v2 and v3 that are not DevelopControls: edited as a whole (a mask
// stroke, a crop drag, a lens-correction toggle), never locked and never changed by the automatic
// settings. Persisted in revision notes ("parts=", recipe_service.hpp) as to_string(); append only.
enum class RecipePart : std::uint8_t {
    ChromaDenoise = 1,     // chroma_denoise; category Detail
    LocalAdjustments = 2,  // local_adjustments (the masks); category Masks
    Geometry = 3,          // geometry; category Geometry
    // lens_correction with its operation version (0: none, the member unused; version 3);
    // category LensCorrections
    LensCorrection = 4,
};

inline constexpr std::array<RecipePart, 4> kAllRecipeParts = {
    RecipePart::ChromaDenoise, RecipePart::LocalAdjustments, RecipePart::Geometry,
    RecipePart::LensCorrection};

// "chroma_denoise", "masks", "geometry", "lens_correction"; "invalid" for other values.
[[nodiscard]] std::string_view to_string(RecipePart part) noexcept;
[[nodiscard]] std::optional<RecipePart> parse_recipe_part(std::string_view text) noexcept;

[[nodiscard]] RecipeCategory category_of(RecipePart part) noexcept;
// The parts of `category`, in RecipePart order.
[[nodiscard]] std::vector<RecipePart> parts_of(RecipeCategory category);

[[nodiscard]] bool same_value(const engine::DevelopSettings& a, const engine::DevelopSettings& b,
                              RecipePart part) noexcept;
void copy_value(const engine::DevelopSettings& from, engine::DevelopSettings& to, RecipePart part);

// Parts whose values differ between `a` and `b`, in RecipePart order.
[[nodiscard]] std::vector<RecipePart> changed_parts(const engine::DevelopSettings& a,
                                                    const engine::DevelopSettings& b);

}  // namespace lucent::app
