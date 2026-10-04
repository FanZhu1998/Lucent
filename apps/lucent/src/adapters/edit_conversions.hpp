#pragma once

// Conversions between the recipe and edit-session types of the application services (lucent::app,
// lucent::engine; ADR 0010) and the shell's editing types (services/edit_service.hpp), and the
// user-facing wording of recipe problems and paste outcomes.
//
// Pure functions of their arguments, safe on any thread; the pixel conversions are meant for worker
// threads.

#include <QImage>
#include <QList>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>

#include <lucent/application/develop_preview.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "services/edit_service.hpp"

namespace lucent::ui::adapters {

// ---- Settings ------------------------------------------------------------------------------------

[[nodiscard]] EditSettings to_edit_settings(const engine::DevelopSettings& settings);

// `settings` over `base`, which supplies what the editor does not change (the schema version, the
// base look and the operation versions; the item's committed settings).
[[nodiscard]] engine::DevelopSettings to_develop_settings(const EditSettings& settings,
                                                          const engine::DevelopSettings& base);

[[nodiscard]] engine::DevelopControl to_develop_control(EditControl control) noexcept;
[[nodiscard]] EditControl to_edit_control(engine::DevelopControl control) noexcept;

// The categories of a category_bit() mask (unknown bits ignored).
[[nodiscard]] std::set<app::RecipeCategory> to_recipe_categories(quint32 bits);
[[nodiscard]] SettingsCategory to_settings_category(app::RecipeCategory category) noexcept;

// ---- State and history ---------------------------------------------------------------------------

// The shell's state of `state`. `store_read_only`: every change is refused (RecipeService::
// read_only_reason()), so no revision can be selected either.
[[nodiscard]] EditState to_edit_state(const app::RecipeState& state, bool store_read_only);

[[nodiscard]] QList<EditHistoryEntry> to_edit_history(const app::RecipeHistory& history);

// Why a photo cannot be edited, for the read-only banner: a recipe this Lucent cannot read
// (`recipe_unreadable`: newer or damaged), a library saved by a newer Lucent, and other errors.
[[nodiscard]] QString describe_recipe_problem(const Error& error, bool recipe_unreadable);

// ---- Camera ------------------------------------------------------------------------------------------

// What the calibration of a probed or decoded file allows for white balance
// (engine::WhiteBalanceModel), with the user-facing explanation when Kelvin is unavailable.
[[nodiscard]] WhiteBalanceInfo white_balance_info(const raw::RawMetadata& metadata);

// A WhiteBalanceInfo saying that the camera's white balance could not be read.
[[nodiscard]] WhiteBalanceInfo unknown_white_balance_info();

// ---- Paste -------------------------------------------------------------------------------------------

// "White balance: ..." for a category the service did not paste into a target. `clipboard` is what
// was pasted; `target` the target's camera when known.
[[nodiscard]] QString describe_paste_skip(const app::PasteSkip& skip, const app::RecipeClipboard& clipboard,
                                          const std::optional<app::CameraCapabilities>& target);

// ---- Frames ------------------------------------------------------------------------------------------

// The clipping overlay of `mask`: highlights (some channel at 255) red, shadows (some channel at 0)
// blue, highlight taking precedence, other pixels transparent (Format_ARGB32_Premultiplied). A
// null image when the mask is empty or memory runs out.
[[nodiscard]] QImage clipping_overlay(const app::ClippingMask& mask);

// The shell's frame of `frame` (pixels converted, histogram and render facts), with its overlay when
// `clipping`; sequence, token and `before` are left for the caller. Null when the pixels cannot be
// converted (out of memory).
[[nodiscard]] std::shared_ptr<EditFrame> to_edit_frame(const app::DevelopFrame& frame, bool clipping);

}  // namespace lucent::ui::adapters
