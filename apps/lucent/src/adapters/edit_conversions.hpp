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
#include <vector>

#include <lucent/application/auto_service.hpp>
#include <lucent/application/develop_preview.hpp>
#include <lucent/application/recipe.hpp>
#include <lucent/application/recipe_service.hpp>
#include <lucent/catalog/jobs.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/develop_geometry.hpp>
#include <lucent/engine/develop_settings.hpp>
#include <lucent/engine/lens_correction.hpp>
#include <lucent/raw/decoded_raw.hpp>

#include "services/edit_service.hpp"

namespace lucent::ui::adapters {

// ---- Settings ------------------------------------------------------------------------------------

[[nodiscard]] EditSettings to_edit_settings(const engine::DevelopSettings& settings);

// `settings` over `base`, which supplies what the editor does not change (the schema version, the
// base look and the operation versions other than the lens correction's, which EditLensCorrection::
// enabled gives; the item's committed settings). Older bases are upgraded (the editor's settings are
// always version 3).
[[nodiscard]] engine::DevelopSettings to_develop_settings(const EditSettings& settings,
                                                          const engine::DevelopSettings& base);

// The version-2 members one by one.
[[nodiscard]] EditGeometry to_edit_geometry(const engine::GeometrySetting& geometry);
[[nodiscard]] engine::GeometrySetting to_geometry_setting(const EditGeometry& geometry);
[[nodiscard]] EditMask to_edit_mask(const engine::LocalAdjustment& adjustment);
[[nodiscard]] engine::LocalAdjustment to_local_adjustment(const EditMask& mask);
// The lens correction of `settings` (its operation version and member) and back.
[[nodiscard]] EditLensCorrection to_edit_lens_correction(const engine::DevelopSettings& settings);
[[nodiscard]] EditLensProfile to_edit_lens_profile(const engine::LensProfileId& id);
[[nodiscard]] engine::LensProfileId to_lens_profile_id(const EditLensProfile& profile);
[[nodiscard]] image::Size to_image_size(QSize size) noexcept;

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

// ---- Automatic settings ------------------------------------------------------------------------------

// What automatic settings did to the revision of `entry` (origin Auto), from its analysis record:
// the confidence, the flags in the user's words (those recommending a review apart from the
// others), the locks report, the final-check attempt and the related-shot group (the record's
// "lucent.auto-record" document is read leniently: a member it lacks is left out).
[[nodiscard]] AutoSummary to_auto_summary(const catalog::AutoAnalysisRecord& record, const app::HistoryEntry& entry);

[[nodiscard]] AutoStage to_auto_stage(app::AutoTaskStage stage) noexcept;

// The outcome of an Auto run in the user's words (AutoOutcome::message and the flags it reports).
[[nodiscard]] AutoOutcome to_auto_outcome(ItemId item, const app::AutoTaskResult& result);

// Why Auto did not save anything (Cancelled, "app.auto.check", "app.auto.support", "app.auto.busy",
// LimitExceeded, a read-only or unreadable recipe, ...).
[[nodiscard]] QString describe_auto_error(const Error& error);

// ---- Paste -------------------------------------------------------------------------------------------

// "White balance: ..." for a category the service did not paste into a target. `clipboard` is what
// was pasted; `target` the target's camera when known.
[[nodiscard]] QString describe_paste_skip(const app::PasteSkip& skip, const app::RecipeClipboard& clipboard,
                                          const std::optional<app::CameraCapabilities>& target);

// ---- Geometry and masks (the EditService helpers; engine/develop_geometry.hpp) ----------------------

// `frame`: the geometry's frame F in the upright image (empty: the whole image; ADR 0017). QUA-05:
// crops are normalised in F, so a version-2 recipe of a file with a camera area maps through it.
[[nodiscard]] GeometryMap geometry_map(const EditGeometry& geometry, QSize upright_size, QRect frame = {});
[[nodiscard]] bool geometry_crop_inside(const EditGeometry& geometry, QSize frame_size);
[[nodiscard]] EditCrop geometry_largest_inside_crop(double straighten_degrees, QSize frame_size, double aspect);
[[nodiscard]] EditGeometry geometry_turned(const EditGeometry& geometry, GeometryTurn turn);
[[nodiscard]] QList<QPolygonF> mask_outline_of(const EditMask& mask, QSize upright_size);

// The frame F of a develop report's settings in its full-resolution upright image (engine::
// geometry_mapping() of report.settings at full size: the camera's area for geometry version 2 when
// the stage one has one); an empty rectangle when it cannot be made.
[[nodiscard]] QRect full_geometry_frame(const engine::DevelopReport& report);

// The weight overlay of `adjustment` (shown even when disabled) for a frame rendered with
// `mapping` (the frame's own GeometryMapping: its upright image, frame F and geometry), tinted
// (premultiplied ARGB, alpha proportional to the weight): laid over the frame's output at most
// `max_long_edge` on its long edge, each overlay pixel's centre mapped back to the upright image
// through that mapping and the weight sampled there (bilinear) from engine::mask_weights() made at
// the overlay's density, at most kMaxMaskOverlayPlaneEdge on its long edge. So the overlay sits
// exactly on the frame (the crop is not rounded at the overlay's scale) and keeps its resolution for
// a small crop. 0 outside the image (a straightened frame's corners). Null for an invalid mask, or
// when memory runs out.
inline constexpr int kMaxMaskOverlayPlaneEdge = 2048;
[[nodiscard]] QImage mask_overlay_image(const engine::LocalAdjustment& adjustment,
                                        const engine::GeometryMapping& mapping, int max_long_edge = 1024);
// The same for `geometry` on an upright image of `upright` pixels whose frame is all of it.
[[nodiscard]] QImage mask_overlay_image(const engine::LocalAdjustment& adjustment,
                                        const engine::GeometrySetting& geometry, image::Size upright,
                                        int max_long_edge = 1024);

// ---- Lens corrections (W13, ADR 0023) ----------------------------------------------------------------

// What a render did for the lens (the record's plan) in the shell's terms.
[[nodiscard]] LensInfoPtr to_lens_info(const engine::LensCorrectionRecord& record);

// The point mapping of a render's lens correction (the plan's LensGeometryMap, full-resolution upright
// pixels); null when the plan resampled nothing.
[[nodiscard]] std::shared_ptr<const LensPointMap> lens_point_map(const engine::LensCorrectionPlan& plan);

// The profiles of `profiles` (engine::LensDatabase::profiles(), in its order) whose maker and model
// hold every word of `text` (case ignored, '/' ignored so "f4" finds "f/4"; no word: every profile),
// at most `limit`: first those in which every word starts a word of the name, then the others (each
// in the database's order).
[[nodiscard]] QList<LensProfileEntry> lens_profile_search(
    const std::vector<engine::LensProfileSummary>& profiles, const QString& text, int limit);

// ---- Frames ------------------------------------------------------------------------------------------

// The clipping overlay of `mask`: highlights (some channel at 255) red, shadows (some channel at 0)
// blue, highlight taking precedence, other pixels transparent (Format_ARGB32_Premultiplied). A
// null image when the mask is empty or memory runs out.
[[nodiscard]] QImage clipping_overlay(const app::ClippingMask& mask);

// The shell's frame of `frame` (pixels converted, histogram and render facts, the geometry and mask
// outlines of its render, its lens correction and point mapping), with its overlay when `clipping`;
// sequence, token, `before`, `uncropped` and the mask overlay are left for the caller. Null when the
// pixels cannot be converted (out of memory).
[[nodiscard]] std::shared_ptr<EditFrame> to_edit_frame(const app::DevelopFrame& frame, bool clipping);

}  // namespace lucent::ui::adapters
