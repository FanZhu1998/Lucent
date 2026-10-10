# ADR 0023: Lens-profile corrections: Lensfun as a private engine dependency, a verified lens database, exact matches only, and DevelopSettings v3

- **Status:** Accepted (2026-10-10, after the first build and test run, W13 build-and-test phase;
  see "Verification"). The design held: the engine phase's code compiled under MSVC `/W4 /WX`
  without a change, Lucent's model matches Lensfun 0.3.4's `lfModifier` to 1e-4 pixel, and the
  corpus matched exactly as section 11 predicted. One deviation (the memory estimate, decision 5),
  the measured limits of the database's lateral-CA calibrations and the follow-ups the application
  owes are recorded under "Verification" and "Consequences". A release package is blocked until the
  GLib and GNU libintl ports stop compiling build-machine folders into their DLLs ("Verification",
  the package).
- **Date:** 2026-10-10
- **Workflow:** W13 (Lensfun lens corrections), engine phase; verified and accepted in the
  build-and-test phase
- **Amends:** ADR 0010 and ADR 0015 (DevelopSettings version 3: the `lens_correction` member and
  operation; stage two gains a first operation; pipeline version 4), ADR 0014 decision 8 (the
  "future Lensfun stage" now exists), ADR 0015 decision 2 (masks stay in the uncorrected frame U;
  the local-adjustments operation version does not change), ADR 0017 (the camera's image area is
  applied after the correction), ADR 0018 (a second bundled asset, third-party data with its own
  licence; the qualification basis changes), ADR 0003 and ADR 0009 (new LGPL DLLs and a CC BY-SA
  data asset in the package)

## Context

Blueprint section 5, step 4: "Correct optics in the appropriate sensor/color domain. For the
initial conventional Bayer route, use linear camera RGB for profile-based vignetting and
channel-specific lateral chromatic aberration correction, combine geometric resampling where
practical, and then transform camera color. Required DNG operations follow their specified
stage/order instead of this generic route. Record the geometry mapping, normalize orientation
consistently, and never apply the same DNG/vendor correction again through Lensfun." Section 3: "An
optional lens-profile miss can leave correction off." Section 5, "Noise, masks, and geometry": masks
"transform predictably through lens correction, crop, rotation, and resizing". Section 8: recipes
carry "camera/lens calibration" and operation versions; "Never interpret a newer unknown operation
as a no-op"; assets are versioned data, verified before activation. Section 9 and Appendix C:
Lensfun is "Selected with separate library/database obligations" (LGPL-3 library, GPL-3 tools,
CC BY-SA 3.0 database), and its assets ship in the installer. The user approved adding Lensfun on
2026-10-09.

Facts verified for this decision (2026-10-10):

- **The library.** The vcpkg port at the pinned baseline is `lensfun` 0.3.4 (port version 2), shared,
  built without its tools (`BUILD_LENSTOOL=OFF`) or Python module, and depends on `glib` 2.90.0,
  which brings `gettext-libintl` 0.22.5, `libiconv` 1.19, `pcre2` 10.47, `libffi` 3.8.0, `dirent`
  1.26, `zlib`, and as build tools `gettext` 0.22.5 (host, `tools` feature), `vcpkg-make` and
  `vcpkg-tool-meson`. Its public header includes only `<stddef.h>`. Lensfun 0.3.4's source (tag
  `v0.3.4`, commit `101c745e`) was read for every convention below:
  - `lfModifier` (`modifier.cpp`, `mod-coord.cpp`, `mod-subpix.cpp`, `mod-color.cpp`): coordinates
    are normalised by `NormScale = 2 / (min(W, H) - 1) * k`, with pixel centres at integers and the
    centre at `((W - 1) / 2, (H - 1) / 2)` plus the lens's centre offset; `k` maps the image's sensor
    to the calibration's through the crop factors and aspect ratios. `ApplySubpixelGeometryDistortion`
    applies the coordinate callbacks by priority (scale, then distortion) and then the subpixel
    callback (lateral CA) on the distorted position; vignetting (`pa`) divides by `1 + k1 r^2 + k2
    r^4 + k3 r^6` with `r = 1` at the calibration sensor's corner. `GetAutoScale` searches eight
    border points with Newton's method and keeps a 1 per mille margin (1.001 for lateral CA alone).
  - Its row API steps the normalised position by adding `NormScale` in float for every pixel, so
    along a long row its positions drift away from those of single-point calls
    (`LensModel.LensfunsRowApiDriftsAlongALongRow` measures up to 0.145 pixel on an 8192-pixel row
    at 24 mm).
  - `lfLens::Interpolate*` return the nearest calibration outside the calibrated focal lengths
    (extrapolation by clamping); vignetting interpolates by inverse-distance weighting over focal
    length, `4 / aperture` and `0.1 / distance`.
  - `lfDatabase::FindCamerasExt` and `FindLenses` write each candidate's `Score` into the database's
    own objects, and `lfLens::GuessParameters` (called by `FindLenses`) switches `LC_NUMERIC` with
    `setlocale`: neither is safe to call concurrently. `lfDatabase::Load(errcontext, data, size)`
    parses XML from memory; it never needs to open a file.
- **The database.** Lensfun 0.3 reads data format version 1 (`LF_MAX_DATABASE_VERSION 1`). The
  Lensfun project publishes the database in every format for `lensfun-update-data` at
  `https://lensfun.github.io/db/` (`versions.json` `[1791482221, [0, 1, 2], []]`, i.e. 2026-10-08
  17:57:01 UTC). The archive `version_1.tar.bz2` (434 744 bytes, SHA-256
  `97c9f8059accae0128a413bc610a04a4548e8da2764a6d3d60d82bf917639457`, retrieved 2026-10-10) holds 56
  XML files (1057 cameras, 1571 lenses). The URL is not immutable, so the source is pinned by commit
  too: running `tools/update_database/generate_db.py` of lensfun master commit
  `43f9e001ab66c4fcdd4f400463f13b72f94b2288` (2026-10-08 19:57:01 +02:00, the commit whose timestamp
  `versions.json` carries) on that commit's `data/db` reproduces all 56 files byte for byte (lxml
  6.1.3). The `release_0.3.x` branch's own `data/db` (last updated 2023-07-12) is three years older
  and lacks lenses of the corpus. The database licence is CC BY-SA 3.0 Unported
  (`data/COPYING.CC_BY-SA_3.0` of the same commit, SHA-256
  `3f941b3b89cf7b8370ceb83cc76d2120d471b58735d8ca60238a751a48d7f72f`).
- **The decoded metadata** (contract 1.4) carries the camera make and model as LibRaw reports them
  and normalised, the lens make and model (`LensMake`, else LibRaw's maker-note lens name), the
  lens's focal range and maximum apertures, the focal length, the f-number and the 35 mm equivalent
  focal length. It carries no lens identifier number and no focus distance.
- **What the files already correct.** `carried_lens_corrections()` (ADR 0014 decision 8) reports
  distortion for an applied WarpRectilinear, lateral CA when its coefficient sets differ per plane,
  vignetting for FixVignetteRadial or a List 2/3 GainMap over the whole image. Non-DNG raw data is
  uncorrected (maker-note correction parameters are metadata, never applied by LibRaw).

## Decision

### 1. Lensfun is a private dependency of `lucent_engine`

`vcpkg.json` gains `lensfun`. The engine links `lensfun.lib` privately (`src/engine/CMakeLists.txt`,
an imported target found with `find_path`/`find_library`: the port installs no CMake package); one
translation unit, `lens_correction.cpp`, includes `<lensfun/lensfun.h>`, and no Lensfun type appears
in any Lucent header (`lens_correction.hpp` wraps everything: `LensDatabase` is a pimpl). The
library ships unmodified as `lensfun.dll` with the GLib DLLs it imports (ADR 0003: replaceable LGPL
DLLs). Every Lensfun call that searches or reads the database runs under one mutex per database.
Lucent runs in the C locale, so Lensfun's temporary `LC_NUMERIC` switches change nothing observable.

### 2. The lens database is a verified, versioned asset

`assets/lens-database` (asset id `lens-database`, version 1.0.0, content `lensfun.database` version
1) holds the 56 XML files of the archive unmodified, the licence text `COPYING.CC_BY-SA_3.0` and
`provenance.json` (the archive's URL, size, SHA-256 and timestamp, the generating commit and script,
the retrieval date, "modifications: none"). `tests/tools/lens_database_asset.py` rebuilds the folder
from the archive and the licence text and checks every SHA-256 (`--check` compares). The manifest is
pinned into the build like every bundled asset (ADR 0018). `lucent_assets` gains
`load_lens_database()` (`lens_database.hpp`): find, verify (pin, sizes, hashes; per-user data packs
refused), then hand exactly the verified bytes of the listed XML files, in the manifest's order, to
`engine::LensDatabase::load()`; Lensfun never reads a file itself. The engine receives the database
from its caller through `DevelopOptions::lens_database` (the engine sits below `lucent_assets`); the
database's identity (asset id, version, manifest SHA-256) is recorded in every plan.

Because the files are unmodified and kept apart from Lucent's code, Lucent distributes the database
as part of a collection, not as an adaptation: the share-alike terms bind the database alone. The
licence bundle carries the CC BY-SA 3.0 text (`licenses\lens-database\`), the credit line
(`THIRD-PARTY-NOTICES.txt`, "Data") and the provenance; the SBOM gives the asset's files that licence
and describes the upstream archive as a package (decision 9).

### 3. Matching never guesses (`LensDatabase::match`, `kLensProfileMatchVersion` 1)

From the metadata (`lens_query()`):

- **Camera:** Lensfun's `FindCamerasExt` (strict: every word of the file's name occurs in the
  database's) for the make and model as reported and as normalised; the camera is identified when one
  of its maker spellings and one of its model spellings equal the file's under Lensfun's own name
  comparison (case, leading, trailing and repeated white space ignored), and all such entries agree
  on mount and crop factor.
- **Lens:** Lensfun's `FindLenses` for that camera (its mount and the mounts compatible with it; no
  calibration made on a smaller sensor), strict, with the file's lens maker when it names one (retried
  without it). A candidate is *the same lens* when one of its name spellings and the file's lens name
  hold the same words, by Lensfun's word rules (digit runs with '.', punctuation runs, letter runs,
  case folded; a lone punctuation mark or a lone "f" is not a word), once the words of the lens maker
  are removed from both; and the database's focal range equals the file's within 1 % when the file
  records one. A fixed-lens camera (its mount is not one of the database's mounts, and exactly one
  lens has it) matches that lens when the file names none.
- **Kinds:** *Exact* when the camera is identified, exactly one lens (by name) is the same lens, the
  file names no other lens maker, and the file's 35 mm equivalent focal length agrees with the
  camera's crop factor within 6 % (otherwise the image is a crop mode of the sensor, and a profile for
  the whole sensor would correct it wrongly; a file without a 35 mm equivalent focal length is taken to
  cover the whole sensor, and the reasons say so). *Fuzzy* when Lensfun returns candidates (strict, or loose
  when strict returns none) but the rule does not single one out. *None* when the database has no
  candidate or the file names no lens. Every result lists its reasons and at most five candidates with
  Lensfun's scores. Several calibrations of the exact lens for different sensors: the one with the
  largest crop factor Lensfun accepted for the camera.

A word *subset* is deliberately not exact: a file's "FE 24-70mm F2.8 GM" would otherwise take the
profile of the "FE 24-70mm f/2.8 GM II", a different lens.

### 4. What is corrected (`plan_lens_correction`, `LensCorrectionPlan`)

Per correction, in order of precedence:

1. **Disabled** by the setting (or lens-correction operation version 0, decision 6);
2. **CarriedByFile**: the file's applied opcodes already correct it, never applied twice (ADR 0014,
   `carried_lens_corrections()`). This phase widens one of its rules: a GainMap of OpcodeList 2 or 3
   is a vignetting correction when its area reaches every edge of the image within its row and
   column pitches (before: the whole rectangle). With pitch 1 nothing changes; the lens-shading maps
   phones write per Bayer phase (pitch 2, starting at row or column 1, ending one short of the bottom
   and right edges, as in the corpus's bq Aquaris U Plus DNG) now count;
3. **Withheld**, automatic mode only: the file carries any lens correction of its own, so no
   third-party profile is mixed in automatically (Lensfun's calibrations describe the uncorrected
   optics; after a vendor warp its vignetting and lateral-CA positions no longer fit exactly);
4. **NoProfile**: no database, a match that is not exact (automatic), a named profile the database
   does not hold, or no crop factor for the image;
5. **NotApplicable**: lateral CA on monochrome data, or a stage-one image that is a part of the frame
   (an analysis crop: the model is centred on the whole frame);
6. **NoCalibration**: the profile has no calibration of that kind, or none that covers the
   photograph: the focal length within the calibrated focal lengths (1 %; any focal length for a
   prime), for vignetting also the f-number within the calibrated apertures (2 %) and a known
   f-number; a profile calibrated on a smaller sensor than the image's; a model Lucent does not
   implement; a distortion model that folds back within the image or a vignetting model that is not
   positive and bounded (gain at most 64) over it;
7. **Applied**, with the calibration Lensfun interpolates for the focal length (vignetting: for the
   f-number and 1000 m, Lensfun's "infinity" entries, since the focus distance is unknown).

The image's crop factor is the database camera's; for a named profile in a crop mode, the file's 35
mm equivalent focal length gives it (recorded with its basis).

### 5. The model and its application (operation version 1)

The model is Lensfun's, evaluated in double precision per pixel by Lucent (`lens_model.cpp`): the
normalisation of `lfModifier`'s constructor, the scale (below), the distortion models `poly3`,
`poly5`, `ptlens`, lateral CA `linear` and `poly3` applied to the distorted position, vignetting `pa`
at each channel's source position. Lensfun is the reference: the unit tests
compare every position with `ApplySubpixelGeometryDistortion` and every gain with
`ApplyColorModification` called point by point (decision 10). Lucent does not call the row API for
the correction itself because of its float drift (Context) and because a per-point evaluation of a
handful of polynomials is cheaper than the call overhead.

The scale starts from Lensfun's `GetAutoScale` (recorded as `LensGeometryMap::lensfun_scale`).
Lensfun checks eight boundary points of a frame it takes to be symmetric about the lens centre, for
the distortion alone, with a fixed 1 per mille for lateral CA. Lucent checks the pixel centres of the
whole boundary in every channel against the image's outermost pixel centres (Lensfun's own
criterion) and raises the scale to the smallest one that keeps them inside, with Lensfun's 1 per mille
margin, where Lensfun's does not: a lens centre offset (computed with a transcription of the
formulas, the synthetic 18 mm test lens would read about 0.07 pixel beyond the image at Lensfun's
scale) or lateral CA beyond 1 per mille at the corners. For a symmetric profile the scale is
Lensfun's (the model test checks equality).

The model's frame is stage one's full-resolution image in **stored orientation** (after the DNG
opcodes, before the camera orientation): a lens centre offset keeps its sensor axes, and a portrait
photograph is corrected like a landscape one. Positions of a proxy are full-resolution positions
scaled per axis, exactly as the proxy downscale scales them, so a proxy is corrected with the same
model as the export.

The correction is **fused into stage two's first pass** (`develop.hpp`, step 0), in white-balanced
linear camera RGB (white balance is a per-channel gain; resampling and the vignetting gain commute
with it): for every output pixel, each camera channel is read at its own source position with the
Keys cubic convolution kernel a = -0.5 (Catmull-Rom, 4 x 4 taps, double sums, taps beyond the image
clamped to its edge pixels: the DNG warps' kernel, ADR 0014), divided by the vignetting at that
position, and then white-balance correction, the highlight clip, camera colour, the working space
and exposure follow in the same pass. One resampling serves distortion and lateral CA. Vignetting
alone does not resample. Without resampling the pass still works in place; with it the consuming
export render writes a new buffer and releases stage one's afterwards (`DevelopMemoryEstimate::
export_peak` counts it: the estimate plans the correction with `DevelopOptions::lens_database` as
the render does, and maps the masks' supports through its geometry for the local-tone base layer;
"Verification", deviation 1). `apply_lens_correction()` exposes the same computation on its own (tests,
tools).

Why stage two and not stage one: the plan depends on the settings (the enable flags, a named
profile), and stage one is the cache an edit session keeps per white balance; in stage two, toggling a
correction costs one pass on the proxy instead of a new decode and demosaic, previews and exports use
one code path, and stage-one consumers (Auto's analysis, the editor) need no new cache key. The first
pass is where the blueprint's order puts it: after demosaicing and OpcodeList3 (stage one), before
camera colour.

Values: Lucent does not clip the corrected camera values; the first pass's highlight clip (the
target's lowest white-balanced saturation level) follows, so a corner brightened above the clip by
the vignetting gain is clipped there, exactly as after a DNG FixVignetteRadial in OpcodeList3.

### 6. DevelopSettings version 3 and the upgrade

`kDevelopSettingsVersion` is 3: `DevelopSettings::lens_correction` (`LensCorrectionSetting`:
`distortion`, `lateral_chromatic_aberration`, `vignetting` flags; `profile`, std::nullopt for
automatic or a `LensProfileId`) and `DevelopOperationVersions::lens_correction`
(`kLensCorrectionOperationVersion` 1; `kOldestLensCorrectionOperationVersion` 0 means *none*).

- **The default** (new photos, `DevelopSettings{}`): automatic, every correction enabled, operation
  version 1; corrections are applied only for an exact match of a file that carries none of its own.
- **A profile override** names a Lensfun lens by `LensProfileId` (maker, model, crop factor of the
  calibration: Lensfun 0.3's own uniqueness key, `_lf_lens_compare`; it has no numeric identifier).
  It is applied whatever the automatic match was, never to a correction the file carries.
- **The upgrade:** version-1 and version-2 settings are still valid inputs when they hold none of
  version 3's members, and `upgrade_develop_settings()` converts them to version 3 with operation
  version 0 and the default (unused) `lens_correction`. Version 0 corrects nothing, so **every existing
  recipe renders exactly as before** (decision 10). Justification: section 8 makes a recipe a frozen
  description of a rendering; a recipe made before the operation existed means "no lens correction",
  and turning corrections on for it would silently change edited photos and their exports, and make
  a previously exported photo irreproducible from its recipe. The photographer turns corrections on
  explicitly, which writes operation version 1 (a new revision), exactly as geometry version 1 is kept
  for older recipes (ADR 0017). `is_as_shot()` treats version 0 like the automatic defaults (neither
  is an edit); a disabled correction or a named profile is an edit. `lens_correction_operation_version()`
  and `effective_lens_correction()` give what a value renders with.
- Validation: operation versions above 1 are VersionMismatch; version 0 with a non-default setting,
  an older value holding the member, and profile references outside their bounds (names of 1 to 256
  bytes without control characters, crop factor within [0.01, 100]) are InvalidArgument.

### 7. Frames: masks stay on their content; the camera's area applies after the correction

- **U**, stage one's upright image, remains the masks' frame for every lens setting. The corrected
  canvas **U'** has U's size; a corrected pixel shows U's content at the green channel's source
  position (`LensGeometryMap::source_of`). Stage two evaluates every mask at that position: analytic
  shapes there, brush rasters (made in U as before, so the raster cache serves both) bilinearly at
  it; the base layer of local highlights and shadows is computed on U' over the masks' supports
  mapped into U'. This is the point mapping the DNG opcodes' `GeometryChange` provides (ADR 0014):
  enabling, disabling or changing a correction never moves a mask relative to the content, and the
  local-adjustments operation keeps version 1 (its meaning, "attached to U's content", holds for
  every recipe; ADR 0015 decision 2 anticipated a version change only if U itself changed, which it
  does not).
- `GeometryMapping` gains the lens map (`create(..., lens, full_size)`): `upright_from_output()` and
  `normalized_from_output()` return U positions (through the correction), `output_from_upright()`
  inverts it with a radial Newton iteration (`corrected_of`); `corrected_from_output()` is the affine
  part, which `apply_geometry()` uses; `identity()` is false with a correction, `affine_identity()`
  says whether the geometry step runs. `geometry_mapping(report)` and `geometry_mapping(settings,
  stage_one, options)` build it; the overload without options keeps having no lens part.
- **The camera's image area** (geometry version 2's frame F, ADR 0017) is the same pixel rectangle of
  U', applied after the correction: the DNG SDK applies DefaultCrop to the stage-3 image after the
  opcode lists, whose WarpRectilinear is DNG's distortion correction, and a WarpRectilinear's optical
  centre is relative to the whole ActiveArea. Lensfun's normalisation likewise assumes the frame it is
  given is the sensor's image centred on the optical axis: the whole decoded image is that frame
  (the area is a centred sub-rectangle of it in the corpus, a difference of at most 1 % in scale).
  The user's crop and straighten are relative to the corrected frame: a correction moves the content
  under a fixed crop.

### 8. Records and versions

- `DevelopOperation::LensCorrection` (13) is recorded first in every report (execution order; its
  `version` is the plan's operation version, `applied` whether anything was corrected; parameters:
  the database, the match and its reasons, the profile, the crop factor and its basis, each
  correction's outcome and detail, the scale, the source positions outside the image).
  `DevelopReport::lens_correction` (`LensCorrectionRecord`) holds the whole plan.
- `kDevelopPipelineVersion` is 4 and `kLensProfileMatchVersion` is appended to
  `kDevelopEngineVersions`. The lens database's identity is data, not an engine version: whoever keys
  pixels by a recipe identity (rendered thumbnails) must key them by `LensDatabaseInfo::identity`
  too. `StageTwoCache::Key` gains the plan's revision.

### 9. Packaging

`LUCENT_BUNDLED_ASSETS` adds `lens-database` (installed into `assets\lens-database`, verified by the
audit like every asset). The runtime closure of `lensfun.dll` is copied by the existing dependency
resolution. `installer/license-policy.json` reviews `lensfun` 0.3.4 (LGPL-3.0-only: the README's
"version 3"; its public header also says LGPL-2.0-or-later), `glib` 2.90.0 (LGPL-2.1-or-later),
`gettext-libintl` 0.22.5 and `libiconv` 1.19 (LGPL-2.1-or-later; libiconv's port declares no
licence), all `lgpl: true` (listed in `LGPL-NOTICE.txt` with their sources and recipes), and
`libffi` 3.8.0 (MIT, in case `ffi-8.dll` ships); it lists `gettext` (GPL-3.0 build tools),
`vcpkg-make` and `dirent` as never shipped, and gains an `assets` section: the lens database is
CC-BY-SA-3.0, its licence file, credit line and comment.
`Get-AssetLicences` (`installer/scripts/LucentPackaging.psm1`) reads it; `New-LicenseBundle` writes
`licenses\lens-database\COPYING.CC_BY-SA_3.0` and the "Data" section of `THIRD-PARTY-NOTICES.txt`;
`New-PackageSbom` gives the asset's files (except Lucent's `provenance.json` and the manifest)
`CC-BY-SA-3.0` and adds a package for the upstream archive (download location, SHA-256,
`GENERATED_FROM` relationships).

### 10. Tests

- `lens_correction_test.cpp` (unit, on a synthetic Lensfun database with every model): settings v3,
  the upgrades and bounds; the word rules; exact, fuzzy and none cases (subset names, two makers, a
  disagreeing maker, an unknown camera, a crop mode, a disagreeing focal range, a fixed lens);
  coverage, carried corrections (synthetic DNG opcodes, per-Bayer-phase GainMaps), monochrome,
  version 0, no database; the model against `lfModifier` for five lens/sensor/orientation cases
  (positions, gains, Lensfun's scale and the raised one for an offset centre); the inverse;
  the autoscale filling the frame; round trips (Lensfun's reverse modifier distorts, Lucent corrects;
  per channel through Lensfun's forward model inverted numerically; vignetting through Lensfun's
  reverse `ApplyColorModification`); a proxy against the downscaled full correction; stage two (the
  record first, the report's mapping, masks following their content across a toggle, determinism
  serial against parallel and export against stage two, the stage-two cache, a part of the frame, the
  memory estimate: planned with the options' database, the masks' supports mapped through the
  correction).
- `geometry_version_test.cpp`: the renders pinned by the engine before the camera image area existed
  (W10, commit 3a836ac) are still produced by version-2 and version-1 settings and their version-3
  conversions *with a lens database whose profile matches the scene exactly*; the same settings made
  now (automatic) do correct.
- `lens_correction_fixture_test.cpp`: the corpus table (decision 11), the drone DNGs never corrected
  twice, the exact matches rendered (a file this build cannot decode is noted and skipped: the Nikon
  Z 8 High Efficiency NEFs), and, disabled (run with `--gtest_also_run_disabled_tests`), the cost on
  the R5 (a named profile), the GFX 50S II and the ILCE-7RM5 (exact matches), and the renders for
  inspection (`LUCENT_LENS_DUMP_DIR`: proxies and full-resolution corner crops with all automatic
  corrections, without lateral CA and with none, as NPY files).
- `lens_database_test.cpp` (assets): the pin, verification and loading, a tampered file and a data
  pack refused, another content schema refused, the canonical manifest, the provenance.
- Updated for the new first operation: `develop_test.cpp`, `develop_v2_test.cpp` (13 operations,
  indices shifted), `develop_settings_test.cpp` (version 3, upgrades to lens-correction version 0)
  and `tests/integration/render_tool_tests.cpp` (lucent-render's report lists `lens_correction`
  first, not applied: the tool passes no database).

### 11. The corpus

The 40 fixtures of `fixtures/manifest.json` with the bundled database 1.0.0 and the default
(automatic) settings. **Measured on 2026-10-10 exactly as predicted** for all 40 files (the
prediction was derived before the first build by applying decisions 3 and 4, with a
transcription of Lensfun 0.3.4's camera and lens searches) to each fixture's probe metadata
(`lucent-probe`, LibRaw 0.22.2) and opcode lists and to the database).
`LensFixtures.TheCorpusMatchesAsDocumented` checks this table
(`src/engine/tests/data/lens_corpus_expectations.inc`) and prints the table it measures.

Corrected automatically (an exact match, nothing carried by the file): 15 files.

| Fixture(s) | Camera, lens as the file names it, focal length, f-number | Profile | Applied | Not applied |
|---|---|---|---|---|
| adobe-dng-canon-eos-5d-mark-iii-linear-dng | Canon EOS 5D Mark III, EF70-200mm f/2.8L IS II USM, 70 mm, f/2.8 | Canon EF 70-200mm f/2.8L IS II USM | distortion, lateral CA, vignetting | |
| fujifilm-gfx-50s-ii-raf-compressed | Fujifilm GFX50S II, GF110mmF2 R LM WR, 110 mm, f/2 | GF110mmF2 R LM WR (crop factor 0.79) | distortion, lateral CA, vignetting | |
| nikon-z-6-nef-compressed, -lossless | Nikon Z 6, NIKKOR Z 24-70mm f/4 S, 52 mm, f/6.7 | Nikkor Z 24-70mm f/4 S | distortion, lateral CA, vignetting | |
| nikon-z-8-nef-he, -he-star | Nikon Z 8, NIKKOR Z 14-24mm f/2.8 S, 24 mm, f/8 | Nikkor Z 14-24mm f/2.8 S | distortion, lateral CA, vignetting | |
| fujifilm-x-t4-raf-compressed, -lossless | Fujifilm X-T4, XF16-55mmF2.8 R LM WR, 16 mm, f/3.6 | XF16-55mmF2.8 R LM WR (crop factor 1.529) | distortion, lateral CA | vignetting: the profile has no vignetting calibration |
| leica-m10-dng | Leica M10, Summicron-M 1:2/35 ASPH., 35 mm, f/5.6 | Summicron-M 1:2/35 Asph. | distortion, lateral CA | vignetting: none calibrated |
| sony-ilce-7m4-arw-compressed, -lossless-l, -lossless-m, -lossless-s, -uncompressed | Sony ILCE-7M4, FE 50mm F2.5 G, 50 mm, f/5.6 | FE 50mm f/2.5 G | distortion, lateral CA | vignetting: none calibrated |
| sony-ilce-7rm5-arw-compressed | Sony ILCE-7RM5, FE 50mm F1.2 GM, 50 mm, f/1.2 | FE 50mm f/1.2 GM | distortion, lateral CA | vignetting: none calibrated |

An exact match that corrects nothing: **hasselblad-l2d-20c-dng** (the DJI Mavic 3's camera; its fixed
lens "L2D-20c & compatibles" by the fixed-lens rule). The DNG's OpcodeList3 already corrects
distortion and lateral CA (a WarpRectilinear with three different coefficient sets) and vignetting
(a GainMap over the whole image): all three are CarriedByFile, never applied again, also with the
profile chosen by hand (`LensFixtures.CorrectionsADngCarriesAreNeverAppliedTwice`).

Not corrected, fuzzy (candidates, but no exact match): 9 files.

- canon-eos-5d-mark-iv-cr2-raw, -sraw: the file's lens name is just "24-70mm"; eight database lenses
  contain those words, none has the same name.
- canon-eos-7d-mark-ii-dng-jxl: "150-600mm F5-6.3 DG OS HSM | Contemporary 015"; no database name holds
  "contemporary" and "015" (closest: Sigma 150-600mm f/5-6.3 DG OS HSM | C).
- canon-eos-r5-cr3-craw, -craw-crop-dualpixel, -raw, -raw-crop: "50mm F1.4 DG HSM | Art 014"; the
  database's "Sigma 50mm f/1.4 DG HSM [A]" lacks "art" and "014" (closest by Lensfun's loose search:
  Sigma 50mm f/1.4 EX DG HSM, a different lens).
- om-system-om-1-orf: the database names the camera "OM Digital Solutions OM-1", the file "OM Digital
  OM-1", so it is not identified; the lens "OLYMPUS M.7-14mm F2.8" is a subset of "Olympus M.Zuiko
  Digital ED 7-14mm f/2.8 Pro".
- sony-ilce-7m4-arw-compressed-apsc: an APS-C crop mode of the full-frame body (75 mm equivalent for
  50 mm, crop factor 1.5 against the camera's 1.0).

Not corrected, no match: 15 files.

- No camera in the database: bq-aquaris-u-plus-dng, oneplus-one-dng, dji-fc4382-dng, dji-fc7303-dng,
  insta360-one-x-dng, olympus-xz-10-orf, ricoh-gxr-s10-dng; dji-fc220-dng is "Mavic Pro FC220" in the
  database, the file says "FC220". These files name no lens (except the GXR's "S10 24-72mm F2.5-4.4
  VC", not in the database). The four phone and drone DNGs with lens opcodes keep them recorded as
  CarriedByFile.
- No lens named, interchangeable-lens camera: canon-eos-350d-cr2, sony-dslr-a450-arw,
  pentax-k-3-mark-iii-pef, pentax-k-3-mark-iii-monochrome-dng (whose camera, named by the
  database under the maker "Ricoh Imaging Company, Ltd.", is not identified either).
- A lens the database does not hold: hasselblad-x1d-ii-50c-3fr ("XCD 80": no XCD lens),
  leica-m-monochrom-typ-246-dng ("Summarit-M 1:2.4/5", a name LibRaw reports truncated),
  panasonic-dc-s5m2-rw2 ("SAMYANG 35-150mm F2-2.8").

So 15 of the 40 files are corrected automatically (6 with all three corrections, 9 with distortion
and lateral CA), one is an exact match whose file already corrects everything, and 24 are left
uncorrected, as the blueprint allows for a profile miss. Two of the 15, the Nikon Z 8 High
Efficiency NEFs, are planned from their probe but cannot be rendered by this build at all (LibRaw
0.22.2: "nikon_he_load_raw() unsupported"), so 13 files render with corrections. Every file of the 9
fuzzy ones can be corrected by naming its profile (`LensCorrectionSetting::profile`).

## Verification (2026-10-10)

The engine phase verified, before any build: the Lensfun 0.3.4 sources (tag `v0.3.4`) for every
convention used above; the database archive's SHA-256 and the byte-for-byte regeneration of its 56
files from commit `43f9e001` (`python tests/tools/lens_database_asset.py --check` passes on the
committed folder, run again in this phase); the vcpkg install plan and the declared licences of the
new ports; the corpus's probe metadata and opcode lists; a clang-tidy parse of every changed
translation unit. The build-and-test phase then built and ran everything (MSVC 14.44, the `dev-core`
preset in `build\w13-eng`, the `dev` preset in `build\w13-dev`, the release package in
`build\w13-rel`; Ryzen 5 5500U, 6 compile jobs):

- **Build.** The engine phase's code compiled and linked under `/W4 /WX` in both presets without a
  change (the only compile errors were in test code this phase added). The runtime closure of
  `lensfun.dll` in the build's `bin` folder is `glib-2.0-0.dll`, `intl-8.dll`, `iconv-2.dll` and
  `pcre2-8.dll`; GObject, GIO, GModule, GThread, `charset-1.dll` and `ffi-8.dll` are not imported.
- **The model against Lensfun** (`LensModel.SourcePositionsAndGainsAreLensfuns`, five cases: ptlens
  zoom at a calibrated and an interpolated focal length on a 601 x 399 portrait, poly5 prime, an
  APS-C calibration with a centre offset turned 180 degrees, a 4:3 calibration turned 90 degrees):
  every source position within 9.6e-5 pixel of `ApplySubpixelGeometryDistortion` (gate 0.01), every
  vignetting gain within 2.0e-7 of `ApplyColorModification` (gate 2e-6), Lensfun's `GetAutoScale`
  reproduced exactly, and the offset centre's scale raised (1.00388) as decision 5 says. Lensfun's
  row API drifts 0.145 pixel along an 8192-pixel row (Context).
- **Round trips** (synthetic scenes distorted by Lensfun itself, corrected by Lucent): distortion
  worst 9.1e-5 (mean 1.8e-5) of a signal between about 0.1 and 0.8, over about 172 000 samples per
  lens; lateral CA
  with distortion per channel worst 5.9e-5; vignetting worst 1.5e-7; a 300-pixel proxy corrected
  against the downscaled full-resolution correction worst 1.6e-4, mean 2.9e-5. A radial mask stays
  on its content across a toggle of the correction to 0.004 pixel (the correction moves it 1.9
  pixels there). Version-2 and version-1 settings and their version-3 conversions render the bytes
  pinned before lens corrections existed, also with a database that matches the scene exactly.
- **The corpus** (`LensFixtures`, LibRaw 0.22.2, the bundled database 1.0.0): the measured table is
  section 11's, file for file (the test prints it, with what each file's opcodes carry: vignetting
  for the bq Aquaris U Plus, whose lens-shading GainMaps are split per Bayer phase, the OnePlus One
  and the DJI FC220; distortion for the FC4382; distortion and vignetting for the FC7303; all three
  for the Hasselblad L2D-20c). The two drone DNGs with a camera profile keep
  every carried correction off, also with their own profile chosen (the FC220's distortion and
  lateral CA are then applied, its vignetting is carried), and enabling a carried correction changes
  no pixel. Every exact match renders with no source position outside the image; scales 1.002 (5D
  Mark III 70 mm, GFX 50S II 110 mm, X-T4 16 mm, ILCE-7M4 50 mm), 1.0124 (M10 35 mm), 1.0117
  (ILCE-7RM5 50 mm), 1.0401 (Z 6 52 mm: the Nikkor Z 24-70 f/4 S has 4 % pincushion in the corners
  at 50 mm); corner vignetting gains 2.30 (EF 70-200 f/2.8L IS II at f/2.8), 2.97 (GF110 at f/2) and
  1.69 (Nikkor Z 24-70 at f/6.7). The database loads in 0.14 s (56 files, 1057 cameras, 1571 lenses,
  52 mounts).
- **Inspection of real renders** (`DISABLED_DumpRendersForInspection`, display-linear renders
  converted to sRGB JPEGs, looked at side by side and measured):
  - *Distortion.* On the Z 6 the routed groove across the table, a straight line 600 proxy pixels
    long below the centre, bows 18.0 full-resolution pixels uncorrected and 3.2 corrected (its RMS
    distance from a straight line falls from 0.98 to 0.30 proxy pixel); two vertical curtain edges
    either side of the centre bow 14.8 and 5.1 pixels uncorrected, 5.9 and 1.3 corrected. On the
    M10, whose profile moves the frame's content by at most 1.1 % (in the corners), the building's
    pipes and a window frame, traced over 1300 to 2300 full-resolution pixels, bow -0.7, -0.2 and +5.0
    pixels uncorrected and -1.1, +3.1 and +8.1 corrected: slightly less straight after the correction,
    by an amount at the limit of what this scene's own straightness allows one to judge (not visible
    side by side). The X-T4 at 16 mm (5 % barrel in the corners) visibly gains content at the edges,
    as a barrel correction does.
  - *Vignetting.* The 5D Mark III and GFX 50S II renders, darkened by 1.2 and 1.6 EV in the corners
    wide open, come out evenly lit; the Z 6 likewise at f/6.7.
  - *Lateral CA.* The profiles' predictions agree with the lateral CA measured in the raw CFA data
    itself (sub-pixel shift of the R and B sample planes against G in the corners, relative to the
    centre, by phase correlation): M10 blue +0.61 pixel outward measured, +0.72 predicted, red 0
    and -0.04; ILCE-7RM5 red +0.78 and +0.92; GFX 50S II red -0.26 and -0.19, blue +0.38 and +0.14;
    ILCE-7M4 about 0 and about 0. In the renders, the correction's own effect (all automatic
    corrections against the same without lateral CA, so the geometry is the same) on the fringe
    energy at edges (band-passed log colour difference relative to the band-passed log green) in
    the four corners: 5D Mark III red -3 %, blue -1 %; GFX 50S II -4 %, -7 % (top corners up to -50
    %); ILCE-7M4 -13 %, -2 %; Z 6 -1 %, -3 %; M10 -13 % red, blue -50 % and -56 % in the top corners
    but more in the bottom ones, whose grass makes colour edges of its own; ILCE-7RM5 red -45 % and -63
    % in the top corners, +20 % and +27 % in the bottom ones (over-corrected there), blue +5 %; X-T4
    red **+129 %**, blue -14 %. At 600 % the ILCE-7RM5's magenta and green highlight fringes and the
    M10's blue fringes under bright mortar lines visibly shrink; on the X-T4 a blue fringe gives way
    to a new orange-red one. The X-T4's profile (XF16-55mmF2.8 R LM WR, calibrated on an X-T10)
    magnifies red by 0.13 % at the centre and about 0.06 % in the corners at every focal length,
    while this file's red is about 0.3 pixel *inward* in the corners: the correction moves it 1.1 to
    1.8 pixels further in. Lucent applies the calibration exactly (round trips above); the
    calibration does not fit this camera and file. Where the residual changes sign after the
    correction (the ILCE-7RM5's bottom corners, out of focus at f/1.2), longitudinal CA, which no
    lateral model describes, or demosaicing, which lends green's detail to red and blue before a
    post-demosaic correction moves them, can explain it; post-demosaic correction is the blueprint's
    route, as for DNG's WarpRectilinear in OpcodeList3.
- **Cost** (`DISABLED_BenchmarkTheCorrectionOnTheR5`, the `dev-core` RelWithDebInfo build, the
  machine otherwise idle, two runs of each export and the median of seven proxy renders, measured
  twice with the same result to within 3 %): the export render (full resolution, decode included) of
  the 45 MP R5 file with its lens named (all three corrections) takes 2.1 s instead of 1.0 s, its
  stage two 1.35 to 1.38 s instead of 0.23 to 0.24 s; the 51 MP GFX 50S II (exact, all three) 4.5 s
  instead of 3.1 s, stage two 1.75 s instead of 0.35 s; the 61 MP ILCE-7RM5 (exact, distortion and
  lateral CA) 2.6 s instead of 0.95 s, stage two 2.0 to 2.1 s instead of 0.37 to 0.44 s. That is 25 to
  27 ns per pixel on six cores for the fused resampling (three source positions, three 4 x 4
  Catmull-Rom sums in double and the vignetting gain per pixel). A 2048-pixel proxy's stage two takes
  81 instead of 20 ms (R5), 104 instead of 31 ms (GFX 50S II), 93 instead of 28 ms (ILCE-7RM5). A
  file's first plan on a freshly loaded database takes 1.1 to 1.6 ms, a cached one 2 to 3 us (the
  database keeps the last 64; the render's memory estimate makes the plan its render then reuses).
- **Deviation 1, found by the existing tests: the memory estimate.** The engine phase counted, for
  any version-3 settings with distortion or lateral CA enabled, the corrected RGB image and the
  local-tone base layer over the whole image (it did not know whether a correction applies). That
  failed `DevelopPipelineV2.ManySmallBrushMasksFitTheDefaultBudgetOnALargePhoto` (32 small shadows
  brushes on a 45 MP image: stage two estimated at 1.08 GB where the test allows 0.63 GB, one RGB
  plane and a sixth) and would have refused renders the budget allows. Now `estimate_develop_memory()` plans the correction with `DevelopOptions::lens_database`
  (none: nothing is corrected, the estimate is the version-2 one) and counts the new RGB image only
  for a correction that resamples, and stage two's own check and the estimate map each tone mask's
  support into the corrected image as `apply_local_adjustments()` does
  (`local_adjustments_working_bytes()` takes the correction's geometry). Without settings, a
  database means a profile may resample. Tested by `LensStageTwo.TheExportEstimateCountsTheCorrectedImage`
  and `TheEstimateMapsTheMasksSupportsThroughTheCorrection`.
- **The test suites.** `dev-core` (1979 tests) and `dev` (2238, the UI's included): every test
  passes except nine, as expected from this ADR's version changes: the assets gate
  `BundledCameraSupport.HoldsForThisBuildsDecoderAndPipelines` (by design: the bundled camera
  support database was qualified with develop pipeline 3 and twelve engine versions; see
  "Consequences") and eight application tests whose expectations are those of recipe schema 2 (see
  "Consequences": the application must follow). `LucentRender.MasksGeometryAndDenoisingAreRenderedAndReported`
  (`tests/integration/render_tool_tests.cpp`) failed too, where it pinned the recipe document's
  version to 2; it now expects the current version. Re-run on the final tree (`build\w13-dev`, after
  an interrupted rebuild was completed): 2238 tests, 2172 passed, the same nine failed, 57 skipped
  (the qualification-tier fixtures), no compiler warning. With `LUCENT_TEST_FIXTURE_TIER=all` the
  124 render, qualification and colour-reference fixture tests (`RenderFixture`,
  `QualificationFixture`, `ColourReferenceFixture`; the 57 among them) all pass: no program or
  service passes a lens database yet, so the version-3 as-shot renders of all 40 fixtures still
  meet their gates and qualification expectations unchanged.
- **The package** (`tools\package.ps1 -BuildDir build\w13-rel -StageOnly`, the release preset): the
  layout carries `lensfun.dll`, `glib-2.0-0.dll`, `intl-8.dll`, `iconv-2.dll` and `pcre2-8.dll` (no
  `ffi-8.dll`, `charset-1.dll` or other GLib library: nothing imports them) and both assets; the audit
  passes (every import resolved, the ports gettext-libintl, glib, lensfun, libiconv and pcre2 among
  the shipped ones, `camera-support 2.1.0` and `lens-database 1.0.0` verified against their pins);
  the smoke run passes; the licence bundle has `licenses\lens-database\COPYING.CC_BY-SA_3.0`, the
  "Data" section of `THIRD-PARTY-NOTICES.txt` and the four new LGPL libraries in `LGPL-NOTICE.txt`;
  the SBOM (107 packages, 529 files, 692 relationships) passes its structural check and gives the 57
  database files `CC-BY-SA-3.0` (the manifest and `provenance.json` stay `MIT`) with the upstream
  archive as a package. One packaging fix was needed: `intl-8.dll` is built from the repository's
  overlay port (`cmake/ports/gettext-libintl`, W13's locale-probe preset), whose recipe files the
  SBOM left out (they differ from the vcpkg registry's) and which `LGPL-NOTICE.txt` described as an
  unidentified registry tree. `LucentPackaging.psm1` now identifies an overlay port by its folder
  in Lucent's source at the build's commit (`Get-OverlayPortFolder`; the SBOM's port package and its
  recipe files, the LGPL notice's recipe line and the notice template). The installer script
  self-tests pass (122 checks). Re-run on the final tree with the same outcome: everything before the
  scan passes, and the scan reports the same four places.
  **The builder-path scan fails**, and this phase may not fix it: `glib-2.0-0.dll` contains its
  meson install prefix's locale folder (`<vcpkg root>\packages\glib_x64-windows-lucent\share\locale`,
  GLib's `GLIB_LOCALE_DIR`, of which Windows builds use only the last two components) and
  `intl-8.dll` its configure prefix (the first configuring build tree's `vcpkg_installed`
  folder) three times (`LOCALEDIR` and the relocation prefixes of `--enable-relocatable`): four
  places that name the build machine's user profile and repository. The remedy belongs to the
  ports (`cmake/ports`, outside this phase's files): an overlay of `glib` that compiles a neutral
  `GLIB_LOCALE_DIR`, and a `gettext-libintl` overlay that compiles a neutral `LOCALEDIR` and drops
  the relocation prefixes (built without `--enable-relocatable`; MSYS converts a POSIX path given to
  the compiler, so the neutral value must survive that). Lucent never uses these strings at run time
  (GLib binds its translation domain under its own DLL's folder and uses only the last two components
  of its string). Until then no release package can be made; the scan must not get an exception for
  them (ADR 0009, decision 12).

## Consequences

- **Qualification.** The develop pipeline version and `kDevelopEngineVersions` changed, so the camera
  support database's basis no longer equals this build's: every Verified mode is reported as
  Compatible, unverified until the qualification is run again with lens corrections and the database
  re-exported (ADR 0018; `BundledCameraSupport.HoldsForThisBuildsDecoderAndPipelines` fails until
  then, by design). The qualification's renders use the as-shot defaults, which now correct exactly
  matched lenses once a lens database is passed.
- **The application** (recipes, editor, Auto, thumbnails, `lucent-render`) is not changed by this
  phase and must follow (requests recorded with the phase's report): the recipe document gains
  `lens_correction` and the `lens_correction` operation (version 3; version 2 read and upgraded with
  operation version 0); `kRecipeSchemaVersion` follows the engine to 3 already, so until then a
  version-2 document re-written by the application loses nothing it had but writes `"version": 3`
  without the new member; the services pass `DevelopOptions::lens_database` (from
  `assets::load_lens_database()`), key rendered thumbnails by the database identity, build overlay
  mappings with `geometry_mapping(report)`, and offer the toggles and the profile choice; Auto should
  decide whether its analysis measures with the correction (its native crops are parts of the frame,
  which stage two does not correct).
- **The application's tests** fail until it follows (measured in this phase's `dev-core` and `dev`
  runs): the recipe tests `RecipeDocument.DefaultSettingsHaveAFixedCanonicalTextAndIdentity`,
  `AcceptsAnyJsonNumberSpellingAndOrder`, `Version1DocumentsAreUpgradedOnRead` and
  `NewerVersionsAreVersionMismatchNeverNoOps`, `RecipeServiceTest.Version1SettingsAreKeptAsTheirConversion`
  and `CatalogRecipeTest.AVersion1RevisionIsReadAsItsUpgrade` pin documents, identities and upgrades of
  schema 2 (the engine's current version is 3, and an upgraded value now carries lens-correction
  operation version 0); `PreviewRecipes.RenderedThumbnailsAreKeyedBySettingsAndSurviveTheSession`
  rebuilds the thumbnail key from twelve engine versions (now thirteen); and
  `PreviewEngineTest.RendersASyntheticDngUprightAtFitAndFullQuality` expects twelve operation records
  (now thirteen, the lens correction first). None of them is a change of rendering: version-1 and
  version-2 settings render the same bytes as before (decision 10). They are the application's to
  update with the recipe schema 3 above, not the engine's.
- Memory: an export with a resampling correction holds a second RGB image at full resolution while
  the first pass runs (about 0.5 GiB for 45 MP), within the 4 GiB budget.
- **Lateral CA depends on the calibration's fit.** Lucent applies each calibration exactly. On four of
  the seven inspected files the correction reduces both channels' fringes (two of them slightly), on
  two (M10, ILCE-7RM5) it reduces them in the corners with sharp detail and increases them in corners
  of grass or out-of-focus detail, and on one it adds red fringing: the database's calibrations come
  from one copy of a lens on one camera, sometimes another body than the photograph's, at an unknown
  focus distance, and the X-T4's profile (XF16-55mmF2.8 R LM WR, calibrated on an X-T10) does not fit
  that file ("Verification"). The automatic correction stays on, as the blueprint asks; revisit (an
  automatic lateral-CA correction only for calibrations that agree with the file's own lateral CA,
  measured in the raw planes as the verification did, or lateral CA off by default) when more files
  show such a misfit, and offer the photographer the toggle in the editor (the application's part
  above).
- **Cost.** A resampling correction is the first pass's main cost: about 26 ns per pixel on six
  cores, so +1.1 to +1.6 s for a 45 to 61 MP export and +60 to +70 ms for a 2048-pixel proxy's stage
  two (20 to 31 ms without; "Verification"). Interactive edits of a corrected file therefore run at
  about 10 to 12 instead of 32 to 50 stage-two frames per second on the reference machine. The obvious
  optimisations keep the model and its tests: evaluate the distortion once per pixel instead of once
  per channel (lateral CA is applied on its result), compute the source positions on a coarse grid
  and interpolate them (the map is smooth), and keep the planes' taps in float; measure them against
  this phase's numbers before the application turns corrections on for previews.
- The Nikon Z 8 High Efficiency NEFs are exact matches that this build cannot decode (LibRaw 0.22.2);
  they are corrected once a decoder reads them, with no change here.
- Crop modes are detected only through the 35 mm equivalent focal length. Files that do not record
  it (Canon CR2 and CR3, the Leica M10 DNG, the Adobe-converted DNG) are taken to cover the camera's
  whole sensor, and the match's reasons say so. A Canon R-series 1.6x crop-mode file with an exactly
  matched lens would be corrected as if it were the whole sensor (the corpus's R5 crop files are
  not affected: their lens is a fuzzy match). Revisit when the decoded-RAW contract carries the
  sensor crop (Canon's crop information) or the full sensor size.
- Until the application serialises `lens_correction`, a version-2 recipe that the application
  re-writes (as version 3, without the member) reads back with lens-correction operation version 1
  (the struct's default): the same identity then renders with automatic corrections once a database
  is passed. The application must write and read the member and the operation with version 3 before
  it passes a database (requested with this phase's report).
- The database is as trustworthy as the executable (the pin); updating it is a new asset version
  (`tests/tools/lens_database_asset.py`), reviewed like any data change. Signed data packs remain
  future work (ADR 0018).
- Revisit when Lensfun 0.4 (data format 2, new models such as `acm`) is packaged, when the
  decoded-RAW contract carries a lens identifier or a focus distance, when the application exposes the
  profile choice, or when the qualification measures lens corrections.

## Alternatives considered

- **Correct in stage one, after OpcodeList3.** Proxies would be downscaled from the corrected image,
  but every toggle would rebuild stage one (decode and demosaic), the stage-one cache would need the
  lens setting in its key, and stage-one consumers would have to know about lenses. Rejected for stage
  two's first pass, which is the same place in the blueprint's order.
- **Distortion at the end, with the geometry (one resampling with straightening).** The blueprint asks
  for the corrections in linear camera RGB; lateral CA must be there anyway (camera channels), so the
  red and blue channels would be resampled twice. Rejected.
- **Lensfun's row API for the coordinates.** Its float position drift (Context) and its per-row
  callback overhead; Lucent evaluates the same formulas in double and tests them against Lensfun.
- **Lensfun's fuzzy best candidate** (as some raw developers use it). The blueprint forbids guessed
  profiles; a subset or a best score picks the wrong generation of a lens.
- **Masks in the corrected frame.** Toggling or changing a profile would move them, and every older
  mask would need a conversion. Rejected for the point mapping.
- **The camera's image area as the model's frame.** The optical centre is the sensor's; DNG places its
  warps on the whole ActiveArea before DefaultCrop. Rejected.
- **Corrections on for upgraded recipes.** Silent changes to existing photos and exports; rejected
  (decision 6).
- **Lensfun's `release_0.3.x` branch database.** An immutable git commit, but from 2023-07-12; the
  published version-1 archive of 2026-10-08, reproduced from a pinned commit, is as reproducible and
  current.
- **Bundling the database inside the executable or Lensfun's own search path.** The asset path verifies
  the data against the build's pin and keeps its licence apart; Lensfun's default search would read
  unverified files from the user's profile.
