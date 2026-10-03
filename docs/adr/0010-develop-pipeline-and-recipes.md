# ADR 0010: Develop pipeline, recipes and interactive previews

- **Status:** Accepted
- **Date:** 2026-10-03
- **Workflow:** W6 (Recipes and editor), phase 2 of 4

## Context

Blueprint section 2 ("Detailed individual editing") asks for exposure, temperature/tint where
the calibration permits, shadows/highlights, a monotone tone curve, contrast and saturation, a
histogram and clipping overlays, undo/redo, "reset to as-shot or an earlier recipe", locks for the
optimiser, and copy/paste by category in which "copying a whole recipe across cameras must not
copy camera calibration, sensor black levels, or camera-specific white-balance gains". Section 4
requires that every preview request carries "image ID, recipe revision, ... and generation token",
that "a late result from an earlier slider position must never replace the current preview", and
"display a quick preview, then refine". Section 7 asks for memory admission and that a queued
export keeps its frozen revision. Section 8 defines immutable, versioned recipes: "Never interpret
a newer unknown operation as a no-op. Open incompatible recipes read-only with a clear reason".

Phase 1 of W6 delivered the engine side (`src/engine`): `DevelopSettings` v1 (the shared contract
"lucent.develop" version 1, with per-operation semantic versions), the operations, a
`WhiteBalanceModel` (Kelvin only where the calibration permits it) and a two-stage pipeline
(`develop.hpp`: a cached stage one, a fast stage two, and `render_develop_file` for exports); and
catalog schema v2 (`recipe_revisions`, an item's current revision, undo/redo moves). Until this
phase, previews, exports and `lucent-render` still rendered the W2 path (ADR 0006, provisional
Neutral v0). This ADR records how phase 2 connects them: the recipe format, the revision model,
the editing commands, the interactive preview service and the switch of every render to the
develop pipeline. Phase 3 builds the editor's user interface on these services.

Measured on the reference laptop (Ryzen 5 5500U, 12 GB) with the 45 MP Canon EOS R5 smoke fixture,
`lucent_application_fixture_tests --gtest_filter=*InteractiveEditing*`, four runs on 2026-10-03
while another workflow was building on the same machine (so the upper ends are pessimistic):

| What | Result |
|---|---|
| Open an edit session: decode, demosaic, 2048-pixel proxy, first frame | 0.90 to 1.14 s |
| Transient edit (exposure, shadows, contrast), request() to delivered frame | median 77 to 116 ms (min 75, max 138) |
| of which stage two on the 2048 x 1366 proxy | median 61 to 85 ms |
| of which 8-bit encoding, histogram and clipping mask | 16 to 24 ms |
| 60 Hz slider drag (60 positions, 16 ms apart) | 10 to 14 frames per second; the last position shown 93 to 185 ms after its request |
| Settled white balance 3200 K (refine): quick frame / refined frame | 0.09 to 0.14 s / 1.04 to 1.39 s |
| 100 % view: full-resolution stage one and stage two | 2.0 to 2.8 s |
| Exposure change at 100 %: quick proxy frame / full-resolution frame | 0.12 to 0.14 s / 1.26 to 1.7 s |
| Memory per open item: the proxy (retained) | 32 MiB |
| at 100 %: the full-resolution stage one (retained) | 512 MiB (544 MiB with the proxy) |
| Process working set while open / at 100 % | +44 MiB / about +1 GiB (with stage two's freed working buffers) |
| Fit 2048 preview of an unedited photo (PreviewService, develop export render) | 1.59 s, of which the render 1.46 s |
| JPEG export (render / encode / write and verify) | 1.28 / 3.58 / 0.87 s |

## Decision

1. **One render definition, the develop pipeline, everywhere.** Library previews and Lucent
   thumbnails (`PreviewService`), exports (`ExportService`), edit sessions
   (`DevelopPreviewService`) and `lucent-render` render `DevelopSettings` through the engine's
   develop pipeline, in the order the engine defines (`develop.hpp`, `develop_operations.hpp`):
   - stage one: decode → normalise → white balance → demosaic → orientation (→ proxy downscale);
   - stage two: white-balance correction relative to stage one, camera colour for the target
     white, working space (Rec.2020 / D65) and exposure in one fused pass → local highlights and
     shadows (guided filter) → contrast → tone curve → saturation and vibrance → base look
     "neutral" v1 → display-linear Rec.2020 for the exporter.

   Exports and non-interactive previews use the export render: stage one of kind Export at the
   settings' white balance (highlights clipped before demosaicing, as in W2) at full resolution,
   then stage two (`engine::render_develop_file`). The application's `SourceRenderer::render` runs
   exactly that composition in its two steps, only to check the file's identity between them and
   report progress; a unit test pins its pixels to `render_develop_file`'s bit for bit. A Fit
   preview is the area-average of that full render (W3 rule, unchanged). The W2 path stays for its
   reference comparisons only (`lucent-render --legacy-neutral-v0`, engine tests).

2. **Recipe documents: "lucent.develop" v1, canonical JSON, SHA-256 identity**
   (`application/recipe.hpp`). A recipe is the JSON form of `DevelopSettings` v1: every field
   written, white balance as `{"mode": "as_shot" | "kelvin" | "camera_neutral", ...}`, the tone
   curve as `[x, y]` pairs, locks as control names, and the semantic version of each operation
   and of the base look. The written form is the JSON Canonicalization Scheme (RFC 8785): sorted
   member names, no whitespace, ECMAScript shortest round-trip number formatting (`-0` is `0`), so
   doubles survive a write and read bit-exactly and the identity, `settings_sha256` = SHA-256 of
   the canonical UTF-8 text, is reproducible by any JCS implementation (the default recipe's
   identity is pinned by a test). Equal settings have equal identities whatever revision or
   transient edit they come from; previews and thumbnails are cached by it.

3. **Strict reading; newer recipes are read-only, never no-ops.** A document must be an object
   with `"schema": "lucent.develop"` and an unsigned integer `"version"`. A newer version is
   `VersionMismatch` and nothing else in it is read. In a version-1 document an unknown member at
   any level, an unknown operation name, white-balance mode or lock, a missing member, a wrong
   JSON type, a repeated member name or an out-of-range value is `InvalidArgument`; an operation
   or base-look version other than the one this build implements, or an unknown base look, is
   `VersionMismatch` (made by another Lucent). An item whose current revision cannot be read is
   opened read-only with that reason (`RecipeState::read_only`): previews, thumbnails and exports
   refuse it with the recipe's error instead of rendering other settings, and edits, locks and
   pastes into it fail; undo, redo, selecting another revision, revert and reset remain possible
   because they never interpret the unreadable recipe. A catalog written by a newer schema (ADR
   0008) makes every item read-only with the catalog's reason.

4. **Revision model** (`RecipeService`, `RecipeStore`). Revisions are immutable and never deleted;
   they go with their item when it is removed (the catalog's cascade). An item points at its
   current revision; none means "as shot" (default settings). The store is the catalog for the
   persistent library and an in-memory equivalent with the same semantics for a session library.
   - *Coalescing.* During a gesture the interface calls `set_transient()` per position: validated
     settings held in memory with an increasing transient generation, no revision. `commit()`
     appends exactly one revision when they differ from the current revision's (nothing
     otherwise); `discard_transient()` drops them; `apply()` is both for one-step edits. A pending
     transient edit is not persisted.
   - *Undo/redo with branches.* Undo moves to the parent revision, redo to the most recently
     created child; an edit after an undo starts a new branch and the undone one stays listed and
     selectable (`select_revision`).
   - *Reset and revert are recorded steps.* `reset_to_as_shot()` appends a Reset revision with the
     default settings (locks kept: they instruct the optimiser, they are not photographic values);
     `revert_to()` appends a copy of an earlier revision (note `from=<number>`), so undo returns to
     where the user was.
   - *Locks* are part of the settings, so a lock change is a revision.
   - *Notes* hold `;`-separated `key=value` fields (`changed=` controls, `locks=`, `from=`) that
     the history listing parses; unknown keys are ignored.
   - Appends are conditional on the current revision the service last read; another writer's
     change is an `AlreadyExists` conflict, after which the service shows the newer state.

5. **Copy and paste by category.** Categories: white balance; exposure; tone (contrast, tone curve,
   base look); colour (saturation, vibrance); local (highlights and shadows, the local-tone
   operation; masks join it later). A paste writes one Paste revision per target that changes.
   Camera-specific values never cross camera models: camera-neutral white-balance multipliers
   (relative to one sensor) are pasted only to the same make and model; Kelvin only to a target
   whose calibration supports it (`WhiteBalanceModel::kelvin_available()`, from a probe of the
   file cached per item, or from the edit session's decode); "as shot" and every other category
   are camera independent. Everything not pasted is reported per target with its reason. Locks
   travel with their controls. Kelvin edits are refused (`Unsupported`) on a camera without a
   Kelvin calibration.

6. **Interactive previews: quick, then refine** (`DevelopPreviewService`). An edit session per open
   photo caches stage one: a 2048-pixel proxy of an Interactive stage one (highlights not clipped)
   balanced for the white balance of the request that built it, and, for a settled 100 % view, the
   full-resolution stage one at the settled white balance.
   - *Quick:* every request renders stage two on the proxy. While one is rendering, newer requests
     replace the waiting one (latest wins); the positions in between are never rendered.
   - *Refine,* only for a settled request (the interface marks gesture positions unsettled and the
     committed revision settled): when any camera channel's stage-two correction factor differs
     from 1 by more than 0.5 % (`kDefaultRefineTolerance`), the proxy is rebuilt at the settings'
     white balance and the request rendered again; a Full request builds or reuses the
     full-resolution stage one at that white balance and renders stage two at full resolution.
     Otherwise the quick frame is already final. Frames are marked Quick or Final.
   - *Tolerances,* measured by the engine's fixture test (CIEDE2000 against the export render,
     R5 and DJI DNG): a proxy balanced as the settings: mean < 0.1, p99 < 0.5 (downscaling does
     not commute with the non-linear operations); a 1500 K change corrected in stage two on the
     proxy (while dragging): mean < 0.3, p99 < 2.5; refined proxy: mean < 0.1, p99 < 0.5; refined
     full resolution: mean < 0.01, p99 < 0.05, the remaining difference being the clip placement
     within two pixels of clipped sensor sites. The 0.5 % tolerance keeps the quick frame's
     demosaic error at a CIEDE2000 p99 of about 0.03 by linear scaling. Where no sensor site is
     clipped, a Final 100 % frame equals the export's pixels exactly (application test).
   - *Staleness:* frames carry the item, the request's generation, its settings revision and
     transient generation and the settings SHA-256. A session delivers frames in strictly
     increasing (generation, phase) order and never concurrently: a frame older than one already
     shown is dropped; a newer request cancels a running full-resolution render and any stage-one
     rebuild it makes useless. Failures are reported for the newest request only.
   - *Overlay data:* each frame has its 8-bit sRGB pixels, the histogram and a per-pixel clipping
     mask (a channel at code 255 or 0), computed in one parallel pass so the overlay and the
     histogram's end bins agree.
   - Unedited photos keep the W3 route (camera preview first, then a Lucent render through
     `PreviewService`).

7. **Memory admission for cached images: retained memory** (`task_executor.hpp`). A stage-one
   build is a full-resolution render (a render lease for the engine's stage-one peak, Interactive
   lane). The kept image then becomes a *retained* claim of its own bytes (`AdmissionLease::
   retain_only`): it counts against the budget, so exports see it, but it holds none of the
   `max_concurrent` render places, so a cached proxy or full-resolution image never blocks an
   export and an export never blocks the slider. Proxy stage-two renders claim their working
   memory as retained memory too; full-resolution stage-two renders are render claims.
   `release_full_resolution()` and `close()` return the memory; `stats()` reports it per session.

8. **Exports freeze the recipe and record its identity.** A request without settings takes the
   item's current recipe at submission (`ExportService::submit`), so editing a photo while its
   export is queued or running never changes that export; an unreadable recipe is refused at
   submission. `ExportRecord::recipe` (`RecipeIdentity`) holds the revision and its number, the
   schema and version, the canonical document and its SHA-256, the operation and base-look
   versions and the pipeline version, and the record keeps the source's content hash: the
   blueprint's job identity (source hash + subimage + recipe hash + renderer versions + export
   settings) for W7's durable receipts.

9. **Lucent-rendered thumbnails keyed by settings.** `PreviewService::request_rendered_thumbnail()`
   renders a 1024-pixel Fit thumbnail of the item's current recipe, encodes it as an sRGB JPEG
   (quality 85) and caches it in memory and, for an item with a content hash, in the disk
   `ThumbnailCache` (generator `LucentRender`, version 1) under a key whose variant is the SHA-256
   of the settings identity, the develop-pipeline and downscale versions and the decoder build:
   a changed recipe or engine never shows an old thumbnail, and the next session shows edited
   photos without rendering. The JPEG is written by the exporter's own writer through a scratch
   file (no in-memory JPEG encoder is public yet).

10. **`lucent-render`.** The default render is the develop pipeline with "as shot" settings, or
    the recipe of `--develop <settings.json>` (read strictly; an unreadable file is a usage error
    with the reader's code); `--exposure` replaces the recipe's exposure. `--legacy-neutral-v0`
    selects the W2 path, and `--dump-linear` (the W2 scene-linear image) requires it. The result
    line and the report (both `schema_version` 2) state the pipeline and the recipe identity; the
    report holds the recipe document and the develop report (stage one records, every operation
    with its version, parameters and time).

## Consequences

- Previews, thumbnails and exports of the same settings are one definition; an edited photo's
  thumbnail, preview and export cannot disagree about which recipe they show.
- The look of every photo changes from Neutral v0 to neutral v1 (filmic shoulder, mid-tones
  +0.5 EV); the W2 reference statistics remain valid for the W2 path only.
  `research/tools/render_reference.py` must pass `--legacy-neutral-v0` (it uses `--dump-linear`).
- Renders of unedited photos cost more than W2's (R5 Fit preview 1.6 s instead of 1.0 to 1.1 s):
  the base look, curves and an Export stage one at full resolution. A reduced-resolution decode
  remains the next speed-up for Fit previews and thumbnails.
- Transient edits reach about 12 frames per second on the R5 proxy; the stage two (about 60 ms)
  and the 8-bit encoding (about 10 ms) dominate. A smaller proxy while dragging, or a faster
  encoder, are the levers if phase 3 needs more.
- A 100 % view holds about 0.5 GB of retained memory for a 45 MP photo; exports still run beside
  it within the 4 GiB budget. Phase 3 should release it when the view leaves 100 % for a while.
- The recipe identity is the canonical document's hash, not the stored text's: a document written
  by another tool in another formatting gets the same identity once read.
- Kelvin availability for paste needs a probe of the target file (a few milliseconds each, cached);
  pasting to many photos belongs on a worker thread.
- Phase 3 (editor UI) uses `RecipeService` (commands, history, clipboard), `DevelopPreviewService`
  (frames, histogram, clipping) and `request_rendered_thumbnail` for edited photos' thumbnails
  instead of the camera preview, through the UI adapters; `Backend` already owns these services.
- Needs recorded for other modules: an in-memory JPEG encoder in `lucent_export` (thumbnails
  currently pass through a scratch file); the decoded metadata in `engine::DevelopReport` (the
  application runs `render_develop_file`'s two steps itself to obtain it); an optional scene-linear
  output of the develop pipeline (for a develop-path `--dump-linear`).

## Alternatives considered

- **Member order as declared, or nlohmann's own float formatting.** Either is deterministic in
  this build, but neither is a published canonical form; RFC 8785 lets any implementation
  reproduce the identity.
- **Lenient reading (ignore unknown fields, default missing operation versions).** Rejected by the
  blueprint: an unknown field or operation may change pixels, and a missing version would make a
  recipe's meaning depend on the build reading it.
- **A revision per slider position.** Fills the history with hundreds of revisions per gesture and
  writes the catalog at 60 Hz; coalescing keeps one revision per user action.
- **Reset and revert as pointer moves.** Moving the current pointer back makes undo jump to an
  unexpected place; recorded steps keep undo linear. Pointer moves remain available as
  `select_revision` for the history list.
- **Cancel the running render on every new request** (the W3 channel rule). During a drag every
  render would be cancelled before it finishes and nothing would be shown until the slider stops;
  latest-wins shows the newest finished position while still never showing an older frame after
  a newer one.
- **Hold a render lease for a cached stage one.** With one render place, a session at 100 % would
  block every export and its own refines; retained memory accounts for the bytes without blocking.
- **Thumbnails from the edit session's proxy frames.** Cheaper, but a second definition of the
  thumbnail (proxy downscale does not commute with the operations); the Fit render of the export
  path is the single definition, cached by settings so it is rendered once.
