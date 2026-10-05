# ADR 0015: Geometry, masks and guided chroma denoising (DevelopSettings v2)

- **Status:** Accepted
- **Date:** 2026-10-05
- **Workflow:** W9 part 2 (Masks, geometry, chroma denoise), phase 1 of 4 (engine)
- **Amends:** ADR 0010 decision 1 (stage two gains three operations) and its recipe contract
  "lucent.develop" version 1 (version 2 adds three members)

## Context

Blueprint section 2 asks for "crop, rotate, straighten" and "brush, linear-gradient, and radial
masks with feathering for detailed local work", and says that "Noise reduction starts with the
guided chroma-noise method specified in section 5". Section 5, "Noise, masks, and geometry",
defines that method and constrains the masks:

- chroma denoising runs "after reconstruction and camera-to-working conversion, before local tone,
  learned appearance, and sharpening", derives "luminance and two color-difference planes from
  linear working RGB", filters "the chroma planes with luminance as the guide", reconstructs RGB,
  blends "conservatively" and preserves "the luminance plane"; radius "in full-image pixels",
  epsilon "in the guide's declared squared signal units", parameters and operation version recorded
  in the recipe, a no-denoising option kept;
- masks are stored "in a defined normalized image coordinate system tied to the orientation and
  geometry revision", transform "predictably through lens correction, crop, rotation, and
  resizing", are distinct from "channel-aware sensor validity maps", and "changing a crop cannot
  move a brush stroke to a different mountain or sky region";
- "Downsampling and filtering do not generally commute; specify preview/export tolerances and
  validate native-resolution crops. Tiled processing requires enough halo for the entire composed
  operation, not only one filter pass."

ADR 0010 defines DevelopSettings v1, the two-stage develop pipeline (stage one cached per photo,
stage two per settings change, proxies for quick frames) and the strict, versioned recipe
contract. ADR 0014 records every DNG warp and trim as a `GeometryChange` in stored orientation,
before the orientation stage, so masks and diagnostics can be carried through, and leaves
Lensfun lens-profile corrections for later.

This phase is the engine (`src/engine`); the recipe JSON, the editor services and the user
interface follow in phases 2 and 3.

## Decision

1. **DevelopSettings version 2.** `kDevelopSettingsVersion` is 2 and DevelopSettings gains three
   members, each with its own operation version in `DevelopOperationVersions`:
   `chroma_denoise` (`ChromaDenoiseSetting`, `kChromaDenoiseOperationVersion` 1),
   `local_adjustments` (an ordered list of `LocalAdjustment`, `kLocalAdjustmentsOperationVersion`
   1, which covers the shapes, their coordinate frame, the rasterisation and the local controls)
   and `geometry` (`GeometrySetting`, `kGeometryOperationVersion` 1: straighten kernel, crop
   rounding, quarter turns and flip). Version-1 fields keep their meaning.
   - *Version 1 stays a valid input.* `validate()` accepts `version` 1
     (`kOldestDevelopSettingsVersion`) when the value holds none of version 2's members (all at
     their defaults, including the three new operation versions; anything else is
     `InvalidArgument`), and stage two renders it exactly as its conversion.
     `upgrade_develop_settings()` converts a version-1 value to version 2 (no denoising, no masks,
     identity geometry; nothing else changes) and returns current values unchanged; versions it
     does not read are `VersionMismatch`. Readers upgrade on read, so equal settings have equal
     identities.
   - *Bounds* (`InvalidArgument` beyond them): at most 32 local adjustments, ids at least 1 and
     unique (the editor's stable identity of a mask), at most 256 strokes per brush mask, 1 to
     4096 points per stroke and 65536 brush points in all (a recipe stays a few megabytes); mask
     positions within one image size of the image ([-1, 2]); brush radius 1e-4 to 0.5 and radial
     semi-axes 1e-4 to 2 (fractions of the long edge); linear-gradient ends at least 1e-4 apart in
     x or y; radial angle within +-180 degrees; feather, hardness, flow and opacity in [0, 1];
     local exposure within +-4 EV and the other local controls within +-1; straighten within
     +-45 degrees; the crop ordered inside [0, 1] and keeping at least 0.001 of each side; quarter
     turns 0 to 3; chroma-denoise strength in [0, 1] and radius 1 to 64 full-image pixels
     (default 8, strength 0).
   - `is_as_shot()` also requires no denoising, the identity geometry and no adjustment with an
     effect (`LocalAdjustment::has_effect()`: enabled, opacity above 0, a control not 0).
   - Mask kinds and brush modes are persisted enums with names (`brush`, `linear`, `radial`;
     `add`, `erase`); the mask kind is the alternative of `MaskShape`, so it cannot disagree with
     the shape.

2. **Coordinate frames** (`develop_geometry.hpp`, `local_adjustments.hpp`). In continuous pixel
   coordinates (pixel (i, j) covers [i, i + 1) x [j, j + 1)), at the render scale:
   - **U, upright:** stage one's image, after the DNG opcodes (ADR 0014) and the camera
     orientation, W x H. Masks are defined here, normalised per axis (a `MaskPoint` (x, y) is the
     pixel point (x W, y H)); lengths are fractions of the long edge max(W, H). Per-axis positions
     map exactly to a proxy because the proxy downscale is per axis (box averages of
     W / W' x H / H' pixels); long-edge lengths keep circles round at any aspect ratio.
   - **S, straightened:** U rotated by `straighten_degrees` about its centre (W / 2, H / 2) on
     the same canvas, counter-clockwise as displayed for positive angles.
   - **C, cropped:** the crop rectangle, normalised in S, rounded to whole render pixels
     ([round(left W), round(right W)) and likewise in y, halves away from zero, at least one
     pixel).
   - **O, output:** C with the user orientation, `flip_horizontal` first and then
     `quarter_turns` clockwise (`user_orientation()`), on top of the camera orientation.
   Masks are defined before straightening and cropping, so a crop, a straighten angle or a
   quarter turn never moves a mask relative to the image content; "tied to the orientation" holds
   by construction (U is the camera-oriented image), and the frame's definition is part of the
   local-adjustments operation version: when lens-profile corrections (Lensfun) change U's
   geometry, that version changes and older masks are mapped through the new correction's
   recorded geometry (ADR 0014 decision 7). `geometry_revision()` and `mask_shape_revision()` are
   content hashes (FNV-1a over the operation version and the values, -0 as 0), not counters, so
   they identify a geometry or a shape for caches and overlays without changing the settings'
   identity; caches compare the values themselves on a hit. `GeometryMapping` (setting + upright
   render size) maps continuous upright points and normalised mask points to output pixels and
   back (`output_from_normalized()`, `normalized_from_output()`), exposes the crop rectangle,
   output size and `revision()`, and is what the editor uses to draw overlays and to turn pointer
   positions into brush points; `mask_outline()` gives the shapes' outlines (ellipses, the three
   gradient lines, stroke centre lines) in normalised coordinates. `rotated_clockwise()`,
   `rotated_counter_clockwise()` and `flipped_horizontally()` give the setting after turning or
   mirroring the image as displayed. `crop_inside_image()` and `largest_inside_crop()` (centred,
   any aspect ratio) help the editor keep a straightened crop inside the image; the aspect
   constraint itself is the editor's.

3. **Operation order.** Stage two (`develop.hpp`): 1. white-balance correction, camera colour,
   working space and exposure (fused); 2. **chroma denoising**; 3. local highlights and shadows;
   4. **local adjustments**; 5. contrast; 6. tone curve; 7. saturation and vibrance; 8. base look
   (display-linear Rec.2020); 9. **geometry**. Operations 2 to 8 work on the upright frame U, so
   masks, guided filters and their radii are defined on the whole image whatever the crop, and a
   cropped output is exactly the matching part of the uncropped one. `DevelopOperation` gains
   `ChromaDenoise` (10), `LocalAdjustments` (11) and `Geometry` (12); every render records all
   twelve operations in this order (an identity one with `applied = false`).
   `kDevelopPipelineVersion` becomes 3 (it is one of `kDevelopEngineVersions`, so rendered
   thumbnails are re-rendered once). `DevelopReport` gains `upright_size` (stage one's image) next
   to `output_size` (after the geometry) and the records `chroma_denoise`, `local_adjustments`
   and `geometry`; `DevelopResult::display` is the cropped and turned image, the exporter's input.

4. **Geometry** (`apply_geometry`). Every output pixel centre is mapped back to U. Without
   straightening the crop is a whole-pixel rectangle and the output an exact copy of U's pixels,
   permuted by the orientation (bit-identical values; equal to `apply_orientation()` of the crop,
   unit test). With straightening: separable 4 x 4 Keys cubic convolution, a = -0.5
   (Catmull-Rom, the DNG warps' kernel, ADR 0014), taps beyond the image clamped to its edge
   pixels, sums in double, each channel limited to the range of the four nearest pixels (no
   ringing: a display image's max(R, G, B) <= 1 holds), and output pixels within half a pixel of
   the image's edge or beyond weighted by their coverage clamp(0.5 + d, 0, 1) towards black: the
   canvas a rotated image does not cover is black with an antialiased border, and
   `GeometryRecord::uncovered_pixels` counts such pixels. The geometry runs last, after the base
   look, so every other operation sees the uncropped image.

5. **Masks and local adjustments** (`local_adjustments.hpp`).
   - *Rasterisation* at the render scale from the normalised definition, never by resampling
     another scale's raster, at pixel centres in double precision: linear gradient t along start
     -> end in pixels, coverage 1 - smoothstep(0.5 - f / 2, 0.5 + f / 2, t); radial (elliptical,
     rotated) normalised radius rho, coverage 1 - smoothstep(1 - feather, 1, rho); brush strokes,
     distance d from the pixel centre to the stroke's polyline, alpha flow (1 - smoothstep(h R, R,
     d)), strokes composed in order (Add m + a (1 - m), Erase m (1 - a)), overlapping segments of
     one stroke not accumulating. Every transition is at least one render pixel wide, centred on
     its nominal position (antialiasing at any scale). Feather widths are fractions of the shape
     (so full-image pixels scaled with the render). The weight is
     opacity x (invert ? 1 - m : m), 0 when disabled. These are scalar edit masks on the
     rendered upright image, one weight per pixel for all channels; channel-aware sensor
     validity (the saturation masks and opcode records of stage one) stays in stage one's
     records in sensor coordinates and is never mixed with them.
   - *Controls*, summed over the masks with their weights and each sum limited to its control's
     range; a pixel whose weights are all 0 is untouched (bit-exact). In scene-linear Rec.2020,
     after the global local tone and before the curves: a white shift (log2 channel gains of the
     von Kries adaptation in Rec.2020 from a white 50 mired bluer (temperature +1, warmer) or 50
     DNG tint units greener (tint +1, towards magenta) than D65, normalised to keep a neutral's
     luminance); one luminance gain 2^(E + tone + contrast) keeping channel ratios, with tone the
     global highlights/shadows weights (`local_tone_gain_ev()`) on the guided-filter base layer
     shifted by the local exposure, and contrast a bounded S-curve in EV around the scene level the
     base look shows as middle grey (slope 1 + 0.6 C, at most +-1.2 EV); then saturation around
     the luminance. Under a mask of weight 1 everywhere, local highlights and shadows equal the
     global operation (unit test, 1e-5 relative).
   - *Memory:* linear and radial masks are evaluated analytically (no plane); a linear gradient's
     work is bounded by the image clipped to its covered half-plane. A brush mask is a float plane
     over its strokes' bounding box at the render scale (`MaskRaster`), kept between renders in a
     `MaskRasterCache` (LRU, 256 MiB by default, keyed by shape revision, shape and size; the
     controls are not part of the key) when `DevelopOptions::mask_cache` is set. The highlights
     and shadows base layer (three float planes) is computed only when a mask uses them, over the
     union of those masks' supports plus a halo of twice the radius (two box passes), so it
     equals a whole-image base layer wherever a weight is not 0.

6. **Guided chroma denoising** (`chroma_denoise.hpp`), per pixel, W the white point:
   Y = Rec.2020 luminance; L = log2 max(Y, W 2^-12); guide I = the 3 x 3 median of L; chroma
   c = ((R - Y) / D_c, (B - Y) / D_c) with D_c = max(Y, 0.25 max(|R|, |G|, |B|), W 2^-12);
   q = guided filter of c with guide I (He, Sun and Tang, Algorithm 1, the OpenCV conventions of
   `guided_filter.hpp`, box means of both planes computed jointly), eps = 0.01 EV^2;
   c' = c + strength (q - c); R' = Y + c'_r D_c, B' = Y + c'_b D_c, G' = (Y - 0.2627 R' - 0.0593
   B') / 0.6780, so the luminance is preserved to float rounding (measured 3.8e-8 of the largest
   channel). Strength 0 skips the operation; monochrome data is skipped (no chroma).
   - *The guide is a median.* A pixel's chroma noise is correlated with its own luminance noise,
     and with the noisy log luminance as guide the filter's regression kept that part (flat field:
     chroma noise reduced 4.9 and 7.6 times instead of 23); the median removes most of the
     guide's noise while keeping step edges (it removes one-pixel luminance details, along which
     colour is averaged too).
   - *D_c instead of Y.* Proportional to a colour's brightness, so a hue keeps its colour across
     luminance edges exactly, but bounded for saturated colours of low luminance (a pure blue has
     Y = 0.0593 B): the relative differences stay within +-8, and one saturated pixel no longer
     tints its neighbours (R5 proxy against export: largest CIEDE2000 29 -> 13).
   - *Units and proxies:* radius in full-image pixels; a proxy scales the window,
     r' = max(1, round(((2 r + 1) scale - 1) / 2)) (6 -> 1, 8 -> 2 at scale 0.25, whose pixels
     already average 4 x 4): it halved the R5 proxy-against-export error of scaling the radius
     itself (mean 0.131 -> 0.064, p99 0.90 -> 0.56). eps is in EV^2, scale independent.
   - *Bands:* horizontal bands of 256 output rows (at least four halos) with a halo of 2 r + 1
     rows (two box passes and the median's row), the original rows of the next band's upper halo
     kept aside before a band is written; eight working planes per band
     (`chroma_denoise_working_bytes()`: 103 MiB for a 45 MP image at radius 16, against 1.7 GiB
     for the whole image). Bands equal the whole-image filter to float rounding (unit test, 2e-6).

7. **The denoised image is reusable while the user edits later controls.** `StageTwoCache`
   (`DevelopOptions::stage_two_cache`) keeps one scene-linear image after the fused pass and the
   denoising, keyed by `StageOneImage::id` (a process-unique id that render_stage_one* and
   make_proxy assign; 0 is never cached), the white balance, the exposure and the chroma-denoise
   setting; used only when denoising is applied, by the non-consuming `render_stage_two()`, for
   images within `max_bytes` (256 MiB by default: a proxy, not a 45 MP image). A hit copies the
   kept image and is bit-identical to a render without the cache (unit test).

8. **Memory estimates.** `DevelopMemoryEstimate::stage_two_bytes` is stage two's RGB image plus
   the largest operation's working memory (local tone, denoising bands, brush rasters and the
   base layer, the geometry's output); a new `estimate_develop_memory(metadata, request, settings,
   options)` overload uses the settings, the old one assumes denoising at its largest radius and
   no brush raster. `render_develop` checks the settings-aware estimate; stage two checks its own
   allocation against `memory_budget_bytes` from the settings.

9. **Preview against export.** Full-resolution interactive renders equal the export apart from
   the highlight clip's placement (ADR 0010); proxies differ because filtering and downscaling do
   not commute. Gated tolerances (CIEDE2000, unit and fixture tests):

   | Comparison | Gate | Measured |
   |---|---|---|
   | Mask rasters, proxy (scale 0.25) vs box-downscaled full resolution, feathered | mean < 1e-3, max < 0.04 | mean 3e-6 to 1.2e-4, max 1e-5 to 0.025 |
   | the same, one-pixel hard edges | mean < 1e-3, max < 0.2 | mean 1.4e-4 to 3.3e-4, max 0.06 to 0.14 |
   | Geometry, proxy vs full (synthetic, display-linear) | mean < 0.004, max < 0.02 | passes |
   | R5, full-resolution interactive vs export, 3 masks + denoising | mean < 0.01, p99 < 0.05 | mean 0.000, p99 0.000, max 1.12 |
   | R5 proxy (2048) vs export, 3 masks | mean < 0.05, p99 < 0.3 | mean 0.011, p99 0.12, max 2.4 |
   | R5 proxy vs export, denoising (strength 0.7, radius 6) | mean < 0.1, p99 < 0.8 | mean 0.064, p99 0.56, max 12.7 |
   | R5 proxy vs export, both | mean < 0.1, p99 < 0.8 | mean 0.067, p99 0.60, max 12.7 |

   For comparison, ADR 0010's proxy edit (local tone, curves, colour) measures mean 0.024, p99
   0.165. The editor's settled 100 % view (refine) shows the export's pixels.

## Consequences

- Measured on the reference laptop (Ryzen 5 5500U, 12 GB) with the 45 MP Canon EOS R5 fixture,
  `lucent_engine_fixture_tests --gtest_filter=*MasksDenoise* --gtest_also_run_disabled_tests`
  (`DevelopFixtures.MasksDenoiseAndGeometryOnTheR5` and `DISABLED_BenchmarkMasksAndDenoise`),
  2026-10-05, settings: a radial mask (exposure, saturation, temperature), a linear gradient
  (highlights, contrast) and a brush mask of two strokes, 202 points (shadows, exposure,
  contrast), chroma denoising strength 0.7 radius 6:

  | What | Result |
  |---|---|
  | Stage two on the 2048 x 1366 proxy, defaults | 22 ms median |
  | ... 3 masks + denoising, first render (brush rasterised) | 156 ms |
  | ... the brush raster cached | 150 ms (denoising 71 to 73 ms, masks 51 to 56 ms, base look 11 ms) |
  | ... the denoised image reused as well (a mask or tone control moving) | 74 ms |
  | ... plus straighten 2.5 degrees, crop and a quarter turn | 115 ms (straightening resamples) |
  | Each mask alone | radial 5.6 ms; linear 28.7 ms (12.4 without highlights); brush 23.7 ms (8.6 without shadows) |
  | Denoising alone on the proxy, parallel / one thread | 70 ms / 316 ms |
  | Export render (decode included), 3 masks + denoising | 3.05 s, of which denoising 1.10 s and masks 0.87 s |
  | Brush raster (proxy) / denoising working memory (proxy, radius 1) | 3.4 MiB / 16 MiB |

  Quick frames with denoising stay interactive only while the denoised image is reused (74 ms
  against ADR 0010's 61 to 85 ms stage two); dragging white balance, exposure or the denoising
  itself costs about 150 ms per proxy frame. The guided filter's box passes are memory bound (4.5
  times faster on 6 cores); a subsampled ("fast") guided filter or a native-resolution cache of
  the denoised image are the levers if the editor needs more.
- **Needs for the next phases** (recorded here because this phase does not change them):
  - *Recipes (application, phase 2):* the "lucent.develop" JSON gains `chroma_denoise`,
    `local_adjustments` and `geometry` and the three operation names; reading upgrades version-1
    documents with `upgrade_develop_settings()`; writing writes version 2 (every member); a
    version-2 document without the new members must be refused. Until then the application's
    `kRecipeSchemaVersion` follows the engine to 2: it writes `"version": 2` documents without the
    new members, reads them and version-1 documents as before (both valid and rendered as before),
    and no longer reports a version-2 document as newer. Nine tests outside the engine encode
    version 1 or nine operation records and fail with this phase (`dev-core`, 2026-10-05):
    `RecipeDocument.DefaultSettingsHaveAFixedCanonicalTextAndIdentity` (the pinned default
    identity), `RecipeDocument.AcceptsAnyJsonNumberSpellingAndOrder` (a version-1 document compared
    with version-2 defaults), `RecipeDocument.NewerVersionsAreVersionMismatchNeverNoOps`,
    `PreviewRecipes.RequestsWithoutSettingsRenderTheCurrentRecipe` and
    `ExportRecipes.ARecipeThisBuildCannotReadIsNeverExported` (version 2 used as "newer"),
    `ExportTest.RecordsCarryTheRecipeIdentityAndTheOriginalIsPreserved` (schema version 1),
    `PreviewEngineTest.RendersASyntheticDngUprightAtFitAndFullQuality` (nine operation records),
    and the `lucent-render` integration tests
    `LucentRender.DevelopFilesAreReadStrictlyBeforeAnythingIsRendered` (version 2 used as "newer")
    and `LucentRender.TheDevelopPipelineIsTheDefaultAndReportsTheRecipe` (version 1, nine
    records). Paste categories: masks join "local"; geometry needs
    its own category (a crop rarely belongs on another photo). Locks: none of the new controls is
    lockable yet (Auto does not change them). `lucent-render`'s report should add `upright_size`
    and the three new records.
  - *Editor services:* pass `DevelopOptions::mask_cache` and `stage_two_cache` per edit session
    (proxy renders) and count their bytes as retained memory; use the settings-aware memory
    estimate; `PreviewSummary::rendered_size` should be `DevelopReport::output_size` (it is
    `full_size`); overlays use `GeometryMapping`, `mask_outline()` and `mask_weights()`; brush
    input should simplify strokes (the bounds are 4096 points per stroke).
  - *Automatic settings (analysis):* candidate renders start from the current settings, so a
    recipe with geometry renders cropped images and the native crops (stage-one coordinates) no
    longer line up with the output; the analysis must decide whether it measures the cropped
    frame (map its crops through `GeometryMapping`) or neutralises geometry for its candidates.
    Its native crops are stage-one images of a sub-rectangle: masks are positioned relative to
    the image they are rendered on, so on a crop they land in the wrong place. Until the engine
    can render a stage-one region of a larger upright frame (an offset and the frame size on
    `StageOneImage`, a candidate for phase 2), the analysis must leave local adjustments out of
    native-crop renders (or out of its candidates altogether) and say so in its record.
  - *User interface (phase 3):* crop, straighten and quarter-turn tools, mask drawing and
    overlays, the denoise control, in the order and frames above.
- Not done: luminance noise reduction, a camera-qualified noise estimate for the denoising
  strength (the default stays 0, and the blueprint asks to compare against no denoising and
  inspect coloured fine detail before Auto enables it), Lensfun corrections (separate step, new
  third-party dependency and database terms), masks driven by segmentation, the subsampled guided
  filter, vectorised resampling.
- Revisit when lens-profile corrections change the upright frame, when the editor needs faster
  quick frames with denoising, or when a validated noise model can set the denoising strength.

## Alternatives considered

- **Masks in the output (cropped) frame, or in the straightened frame.** A crop or an angle change
  would move every mask relative to the content, exactly what section 5 forbids; masks in U never
  move and map to any output by an affine transform.
- **Mask positions normalised to the long edge in both axes.** Keeps lengths isotropic, but a
  proxy's per-axis rounding (e.g. 5464 -> 1366 of 8192 -> 2048) maps positions per axis; per-axis
  positions with long-edge lengths keep both exact.
- **A geometry revision counter in the settings.** Two equal geometries reached by different
  edits would differ and change the recipe identity; a content hash identifies the value.
- **Geometry before the base look (scene-linear) or before the masks.** Before the masks or the
  guided filters, a crop would change their results near the crop edge (their radii see a
  different neighbourhood); after the base look the resampled values are display-referred and the
  no-ringing limit keeps them within [0, 1]. Either place is exact for crops and turns.
- **Edge extension instead of black for uncovered canvas.** Silently smears the border; black with
  `uncovered_pixels` recorded, and `largest_inside_crop()` for the editor, is honest.
- **Bilinear or Lanczos-3 resampling.** Bilinear blurs every straightened image; Lanczos-3 needs
  36 taps and rings more; Catmull-Rom is the warps' kernel, interpolating and third-order.
- **Local highlights and shadows added to the global operation's gain on one base layer.** One
  guided filter less, but the sum of two bounded gains can reverse the order of tones (the
  global operation's slope bound holds for |amount| <= 1 only); applied after it on its own base
  layer each stays bounded.
- **Chroma relative to the pixel's own luminance, absolute differences (R - Y, B - Y) with a
  linear-luminance guide, or opponent axes orthogonal to the luminance.** Absolute differences
  with a linear guide make epsilon depend on brightness (dark edges bleed); orthogonal axes only
  decorrelate noise that is independent per working channel, which demosaiced, matrixed camera
  noise is not; relative to Y alone the ratios explode for saturated colours of low luminance.
- **The guide as a box- or guided-filtered luminance.** A 3 x 3 mean blurs every step edge by a
  pixel (colour bleeds into the neighbours); a self-guided pre-filter has the same noise/edge
  trade-off as the filter itself; the median keeps steps.
- **Scaling the denoising radius itself for proxies** (as the local tone does): r x scale rounds 6
  to 2 at scale 0.25, a window twice as wide as the export's; scaling the window halves the error.
- **The subsampled "fast guided filter"** (He and Sun, 2015): 4 to 16 times faster, but another
  definition of the operation whose proxy/export behaviour needs its own validation; kept as the
  next lever.
- **Keeping DevelopSettings at version 1 with an optional extension type.** Two settings types
  through every layer; the version and the conversion are what the forward-compatibility rules
  (section 8) are for.
