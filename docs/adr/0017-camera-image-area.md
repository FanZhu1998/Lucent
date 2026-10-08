# ADR 0017: The camera's image area: decoded-RAW contract 1.4, geometry version 2 and the W2 camera_area stage

- **Status:** Accepted
- **Date:** 2026-10-07
- **Workflow:** W10 (Multi-brand qualification), phase 2: the camera's image area
- **Amends:** ADR 0005 decision 2 (contract 1.4), ADR 0006 sections 1, 5 and 6 (a camera_area
  stage, RenderSettings 1.2, render pipeline 3), ADR 0014 ("still open: DefaultCrop"), ADR 0015
  decisions 1 to 4 (geometry operation version 2; see its amendment of the same date) and ADR 0016
  decision 3 (check suite 2)

## Context

The phase-1 qualification run (ADR 0016) proposed 4 Verified, 2 Compatible-unverified, 25 Limited
and 3 Unsupported modes. 24 of the 25 Limited modes were limited because Lucent rendered the
decoder's whole visible area instead of the camera's image area: "the default crop is the single
largest gap" (ADR 0016, Consequences). Blueprint section 3 asks that a known optional feature which
is unavailable makes a mode Limited, and that "required interpretation steps" are what separates
Compatible from Limited; section 5, step 2 lists "active-area rules" among the sensor normalisation
steps; section 8 requires recipes to be versioned by operation so that "existing recipes" keep
rendering as they were made ("Never interpret a newer unknown operation as a no-op", "Retain ... a
compatible renderer version for old recipes").

Facts verified for this decision (the DNG specification read 2026-10-05; the DNG SDK, LibRaw
sources and the corpus re-checked 2026-10-07):

- **DNG.** DNG 1.4.0.0 (September 2012), DefaultCropOrigin (50719, p. 31): "Raw images often store
  extra pixels around the edges of the final image. These extra pixels help prevent interpolation
  artifacts near the edges of the final image. DefaultCropOrigin specifies the origin of the final
  image area, in raw image coordinates (i.e., before the DefaultScale has been applied), relative
  to the top-left corner of the ActiveArea rectangle." DefaultCropSize (50720) "specifies the size
  of the final image area", default ImageWidth, ImageLength. DefaultUserCrop (51125) is one of the
  "Additional Tags for Version 1.4.0.0" (p. 70, not 1.5): "Specifies a default user crop rectangle
  in relative coordinates ... 0.0 <= top < bottom <= 1.0, 0.0 <= left < right <= 1.0. The default
  values of (top = 0, left = 0, bottom = 1, right = 1) correspond exactly to the default crop
  rectangle". The DNG SDK (AOSP `external/dng_sdk`) applies the default crop to its stage-3 image,
  after the opcode lists: `dng_negative::DefaultCropArea()` rounds the origin and the size
  separately (`Round_int32`) and, when the rectangle overruns the stage-3 image, moves it back
  inside by the overrun; `dng_simple_image::Trim()` (TrimBounds) re-bases the image at the trim's
  origin, so after a TrimBounds the default crop is relative to the trimmed image.
- **LibRaw 0.22.2** (vcpkg source tree). `imgdata.sizes.raw_inset_crops[2]` (doc/API-datastruct.html:
  "item at [0] contains standard crop data parsed from camera metadata (DNG: DefaultCrop* tags,
  other formats: vendor specific tags)", "item at [1] is 'user crop' ... set by DNG DefaultUserCrop
  tag or set via raw aspect ratio tags (e.g. 16:9 aspect on Fujifilm cameras)"). The values are
  raw-frame pixels; Changelog 2021-02-04: "There are no flags to check if raw_inset_crops[Nth] is
  filled; ... check ... ctop/cleft < 0xffff ... ctop+cheight <= sizes.raw_height, cleft+cwidth <=
  sizes.raw_width" and "raw_inset_crops may be fooled by metadata (makernotes), use some
  correctness check before use". Vendor sources: Canon CR3 maker-note crop (src/metadata/canon.cpp),
  Sony raw-IFD DefaultCrop tags and 0x74c7/0x74c8 (tiff.cpp; LibRaw promotes DefaultCrop tags of
  non-DNG files for Sony only), Fujifilm RAF RawImageCropTopLeft / RawImageCroppedSize (fuji.cpp),
  Panasonic sensor borders and crop tags 47-50 (tiff.cpp), Nikon 0x0045, Olympus 0x0612-0x0615,
  Pentax 0x0038/0x0039, Kodak, Phase One CUTRECT. src/utils/open.cpp then moves a left or top edge
  that is not on the CFA period (6 for Fujifilm X-Trans data; 2 for other CFA data unless LibRaw
  holds it in the rotated Fuji layout) inward to the next multiple and shrinks the size by the same
  amount ("adjust crop_inset margins"). `adjust_to_raw_inset_crop()` (src/utils/utils_libraw.cpp)
  would promote a crop to LibRaw's margins, changing the visible area and the CFA phase (and so
  decode parity); its plausibility bound is `cwidth >= int(width * maxcrop)` (likewise the height)
  with the default `maxcrop` 0.55.
- **The corpus** (`LibRawFixtures.CameraImageAreasOfTheCorpus`): 32 of 34 fixtures have an area
  Lucent can read. DNG DefaultCrop for 9 of the 10 DNGs (the Insta360 has none: the specification's
  default, the whole image); LibRaw inset crops for 22 of the 24 other files, all equal to the
  camera's recorded size cited in `expectations.json` (Sony L/M/S and APS-C, Nikon Z 6 and Z 8,
  Canon 5D Mark IV RAW and S-RAW, R5 full frame and 1.6x, GFX 50S II, X-T4, S5 II, OM-1). The R5's 8192 x 5464 lies one column and row
  beyond LibRaw's visible area. The X-T4 RAF declares top 13, which LibRaw moves to 18 (4155 rows
  instead of 4160). LibRaw reads no crop for the Pentax K-3 Mark III PEF and does not read the
  Hasselblad 3FR raw IFD's DefaultCrop tags.

## Decision

1. **Decoded-RAW contract 1.4** (`decoded_raw.hpp`): `RawMetadata::image_area` (`ImageArea`) with
   `source` (`Unknown`, `DngDefaultCrop`, `DngDefaultWholeImage`, `DecoderInsetCrop`; persisted,
   append only), `provenance`, `rect` (continuous raw-frame coordinates, stored orientation),
   `reference` (the rectangle the declaration is relative to: the DNG ActiveArea, or the raw
   frame), `user_crop` and `basis`. Pixel values and the visible area are unchanged; the engine
   applies the area. `check_consistency()` checks it.
   - *DNG* (`dng_image_area()`): the inspector's DefaultCropOrigin and DefaultCropSize of the
     decoded raw IFD, relative to its ActiveArea, kept as stored (rationals, not rounded); without
     both tags the specification's default, the whole ActiveArea (`DngDefaultWholeImage`, rule); a
     lone tag, or a raw IFD of another size than the decoded frame, is `Unknown`. DefaultUserCrop
     (the inspector now reads it; advisory, so a malformed one is skipped with a warning instead of
     failing the inspection) becomes `user_crop` unless it is (0, 0, 1, 1).
   - *Other files* (`image_area_from_inset_crops()`): LibRaw's `raw_inset_crops[0]` when both edges
     and sizes are recorded, it lies inside the raw frame and it keeps at least
     `kMinInsetCropFraction` (0.55, LibRaw's maxcrop, with LibRaw's integer limit) of the visible
     area's width and height;
     otherwise `Unknown` with the reason. `raw_inset_crops[1]` inside it becomes `user_crop`. When
     Lucent can read the file's own declaration and LibRaw's value is exactly its margin-adjusted
     form (`libraw_adjusted_inset_crop()`), the declaration is used (`Provenance::LucentRule`):
     the RAF reader (`read_raf_crop()`, the metadata directory the RAF header points to) restores the
     X-T4's top edge.
   - The user crop is exposed, never applied: it is the initial value of a photographer's crop,
     which may be widened back to the whole area (DNG's definition). Using it as the initial crop
     of a new recipe is the application's decision (not taken here).
   - The contract bump makes every catalogued probe outdated once (ADR 0014 decision 11): the
     library re-reads each file in the background on the first start.

2. **Placement** (`engine/image_area.hpp`, `kImageAreaPlacementVersion` 1). The engine places the
   area in its image after the DNG opcodes, in stored orientation (`place_image_area()`): for a DNG
   relative to the stage-3 image (`DngOpcodeProcessor::stage3_frame()`: the ActiveArea, re-based by
   every TrimBounds applied), origin and size each rounded half away from zero and an overrunning
   rectangle moved back inside (the SDK's rules), then mapped into Lucent's buffer (one row or
   column inside an odd ActiveArea margin) and cut to it; inset crops in raw-frame pixels relative
   to the visible area, cut to it. What was cut is recorded per side (`clipped`; the R5 loses one
   column and row of its declared 8192 x 5464). An area of which no pixel remains is not applied
   (a warning). Stage one turns the placement with the camera orientation and keeps it in
   full-resolution upright pixels (`StageOneImage::image_area`); proxies keep it and scale it per
   axis at render time (`scale_area()`, edges rounded half away from zero). A version-2 recipe's
   pixels depend on these rules, so `kImageAreaPlacementVersion` is one of `kDevelopEngineVersions`
   (develop.hpp), which key the pixels persisted under a recipe identity (rendered thumbnails).

3. **Geometry operation version 2** (`kGeometryOperationVersion` 2; version 1 kept,
   `kOldestGeometryOperationVersion`). The geometry works in a frame F of the upright image U
   (develop_geometry.hpp): all of U for version 1, the camera's image area for version 2 (all of U
   when the file declares none). Straightening rotates F about its centre on F's canvas, the crop is
   normalised in that canvas, coverage (black canvas) is measured against F's edges, and
   interpolation taps may read U's real border pixels outside F. The identity geometry of version 2
   renders exactly the area's pixels of the version-1 rendering (`GeometryVersions` tests:
   bit-identical sub-images). F = U reduces every formula to version 1's bit for bit
   (`GeometryMapping::create(geometry, size)` is `create(geometry, size, whole image)`).
   - *Masks keep their frame, before the camera area.* Masks stay normalised in U (the whole
     upright image, border included) for both versions, and operations 2 to 8 of stage two still
     run on all of U. So a mask is attached to the same content in a version-1 and a version-2
     rendering, pasting masks between recipes of either version needs no conversion, the guided
     filters see the border as context, and a crop or a change of version never moves a stroke.
     The cost: a version-2 mask position near (0, 0) lies in the border outside the output, which
     an editor that maps pointer positions through the frame's `GeometryMapping` never produces
     (the application's frames and overlays do; the Qt editor's own tools do not yet, see
     Consequences).
   - *Versions.* New settings and the as-shot defaults (`DevelopSettings{}`) use version 2.
     A recipe keeps the version it was made with: documents of geometry version 1 read as version
     1 and render exactly as before; version-1 settings (DevelopSettings v1, W6-W8 recipes) upgrade
     to geometry version 1 (they rendered the whole visible area), and render as version 1 even
     unconverted (`geometry_operation_version()`). Every existing recipe identity is unchanged; the
     default recipe's identity moves from 3104dbcc... (now the identity of default settings with
     geometry version 1) to 879de8b7... (pinned in recipe_test.cpp). `is_as_shot()` ignores the
     version (it is a recorded operation version).
   - `geometry_mapping(settings, stage_one)` (and the overload for report sizes) is the mapping
     stage two renders with; `DevelopReport` records `geometry_version`, `geometry_frame` and the
     stage one's `image_area`; the geometry operation record has the version and, for version 2,
     the frame and its source. Stage two refuses an image area on a stage one that is not the whole
     frame at its scale (code that cuts a stage one must drop the area).
   - Memory: stage two's budget check and `estimate_develop_memory()` with settings count the
     geometry's output in the version's frame (the metadata's area placed without the opcodes'
     trims, which only shrink it): with an area, version 2's identity geometry writes a new image of
     the area's size, which version 1's identity does not.
   - Paste: the geometry part carries its operation version (`copy_value`, `same_value`), since a
     crop of version 1 and one of version 2 are relative to different frames.

4. **The W2 render path** (`render.hpp`): stage `camera_area` (12) after the DNG OpcodeList3 stage
   (or demosaicing) cuts the image to the area (`crop_to_area`); every later stage is per pixel, so
   this equals cutting the final image. `RenderSettings` 1.2 adds `apply_camera_area` (default true;
   false records the area with a warning, for comparisons with decoders that render their visible
   area), `RenderReport` records `image_area` and `camera_area_applied`, `MemoryEstimate` the
   `camera_area_peak`; `kRenderPipelineVersion` is 3. `lucent-render --no-camera-crop` sets it for
   `--legacy-neutral-v0` and renders the develop pipeline with geometry version 1 (a usage error
   when the settings crop or straighten within the area); `research/tools/render_reference.py`
   passes it, and the rawpy gates of `lucent_render_integration_tests` render with it, so parity
   stays like for like. The develop pipeline has no switch: the recipe's geometry version decides.

5. **Automatic settings are unaffected.** The analysis evaluates the whole upright image as before
   (it drops the area from its own stage one: the area is part of the geometry ADR 0015 sets aside),
   so its proxies, native crops and appearances line up and its decision is the same for either
   geometry version (`AutoServiceTest.TheCameraAreaDoesNotChangeTheAutoDecision`); results carry the
   recipe's geometry version. The application's full-resolution check maps its image context through
   `geometry_mapping()`, so a version-2 recipe is checked in its real output frame.

6. **Application plumbing.** Editor frames (`DevelopFrame::geometry`, mask overlays) and
   `rendered_full_size()` use `geometry_mapping()`. The batch review note compares the output with the
   catalogued frame's geometry; the catalog does not record the camera's area, so for geometry
   version 2 an output up to `kCameraBorderTolerance` (5 %) smaller per side is not a review note.

7. **Qualification, check suite 2.** The dimensions check measures the develop render's output (the
   area for version 2), a DNG DefaultCropSize counts as applied when the output has its size, the
   orientation check compares the stage-one image with the visible area, the colour check renders
   without the area (like rawpy), and each fixture's report records the declared and applied area
   (support table: "Camera image area"). Expected outcomes (rendered size, state, limitations)
   changed only for the 22 fixtures whose output the area now changes; no fact, status or source
   changed. Every fixture now pins where its area comes from (`dimensions.image_area_source`,
   checked by the loader and the GTest wrapper) and its note says so.

## Consequences

- Run of 2026-10-07 over all 34 fixtures (`docs/qualification/qualification-report.json`, generated
  2026-10-08T03:13Z; check suite 2; every outcome equal to the expectation, whose new states and
  output sizes were recorded before the run): **Verified 14, Compatible, unverified 11, Limited 6,
  Unsupported 3** (phase 1: 4, 2, 25, 3). The camera's area removes 22 of the 24
  `default_crop_not_applied` limitations; every changed output is the camera's recorded size
  (Sony, Canon, Nikon, Fujifilm, Panasonic, OM System) or the DNG's DefaultCropSize.
  - *Limited to Verified (10):* Sony A7 IV compressed, uncompressed, lossless L, M and S and APS-C
    crop, A7R V; Canon EOS 5D Mark IV RAW; Pentax K-3 Mark III Monochrome DNG; Leica M Monochrom
    (Typ 246) DNG. No new fact was needed: each mode's camera image area was already a verified
    fact (manufacturer specification, or the file's own tags read by `file_facts.py`), and the
    evidence rule is unchanged (ADR 0016 decision 2).
  - *Limited to Compatible, unverified (9):* their phase-1 evidence gaps remain: DNG colour that is
    not like for like (DJI FC4382 and FC220, Hasselblad L2D-20c, Leica M10) and a compression mode
    no independent source states (Nikon Z 6 lossless and compressed, Fujifilm GFX 50S II, Panasonic
    S5 II, OM System OM-1).
  - *Still Limited (6):* Fujifilm X-T4 lossless and compressed (X-Trans baseline demosaic; the area
    is applied, 6240 x 4160), the R5 Dual Pixel file, the Canon 7D Mark II DNG (Enhanced image; the
    area is applied), and the Pentax K-3 Mark III PEF and Hasselblad X1D II 3FR, whose area LibRaw
    does not read (`default_crop_not_applied`, source `unknown`).
  - *Unchanged:* the R5 RAW, C-RAW and 1.6x modes (the declared area lies one column and one row
    beyond LibRaw's visible area, so the whole visible area is the area applied), the 5D Mark IV
    S-RAW, the DJI Mini 2 and Adobe linear DNGs (DefaultCrop = the whole image) and the three
    Unsupported modes.
- Every render of a new or as-shot recipe changes (the output is the camera's area); existing
  recipes render byte-identically. Checked on 2026-10-07, and again with the final build on
  2026-10-08, against a build of commit 3a836ac (the engine before this change, from `git archive`): the synthetic renders pinned in
  `geometry_version_test.cpp` (version-1 settings and geometry, proxies, camera orientations, DNGs
  with DefaultCrop tags, odd ActiveArea margins and OpcodeList3 opcodes, the W2 render without the
  area) are that build's output, and `lucent-render` from both builds wrote byte-identical 16-bit
  TIFFs for 7 fixtures (Sony A7 IV ARW, DJI FC4382 DNG, Fujifilm X-T4 RAF, Pentax K-3 Mark III
  Monochrome DNG, Canon 5D Mark IV CR2, Leica M10 DNG, Canon R5 CR3) under two version-1 recipes
  (the identity, and one with exposure, tone, colour, chroma denoising, a radial and a brush mask,
  a 1.5 degree straighten, a crop and a quarter turn) and for the as-shot defaults with
  `--no-camera-crop` against the old build's as-shot defaults: 21 of 21. Thumbnails are rendered
  again once (the as-shot recipe identity and `kDevelopEngineVersions` changed).
- The editor's own crop and mask tools (apps/lucent: `edit_conversions.cpp` `geometry_map()`,
  `geometry_crop_inside()`, `geometry_largest_inside_crop()`, `mask_overlay_image()`; the crop tool and
  mask editor) still build their mappings from the full upright size: for a version-2 recipe of a
  file with a camera area they place crop handles and turn pointer positions into mask points
  relative to the whole frame, off by the area's origin (a few pixels to 44 pixels at full
  resolution in the corpus). They must take the frame from `DevelopFrame::geometry` (or
  `GeometryMapping::frame()`) and pass `frame_size()` to `largest_inside_crop()`; recorded as a
  follow-up for the UI owners.
- The catalog's probe summary does not record the area; exports' review notes use a tolerance
  (decision 6), and library sizes show the visible area until the catalog stores it.
- Files whose area LibRaw does not read stay Limited (Pentax K-3 Mark III PEF, Hasselblad X1D II 3FR):
  reading the 3FR raw IFD's DefaultCrop with Lucent's TIFF inspector (an engine change, like the RAF
  reader) and a per-model area table for files that declare nothing (a data-only profile update,
  blueprint section 8) are possible later extensions.
- The area is always applied by default; a photographer who wants the border back has no control
  yet (a version-1 geometry, or a "show camera border" option, is a UI decision).
- Revisit when a decoder other than LibRaw is added (its crop reading replaces `DecoderInsetCrop`),
  when the DNG SDK is available for a parity check of the placement, or when a corpus file carries a
  TrimBounds or a fractional DefaultCrop.

## Alternatives considered

- **Cut the area in stage one (U = the area).** Masks, guided filters and the local-tone radius
  would see a different image per geometry version, the cached stage one could not serve both, and
  version-1 recipes would need a second stage one. Rejected: the area belongs to the geometry's
  frame, applied last.
- **Masks in the area's frame for version 2.** Masks would move between versions and need a lossy
  conversion when pasted between recipes of different versions; the guided filters' view would still
  be U. Rejected (decision 3).
- **A flag in GeometrySetting.** The analysis and the paste rules compare geometry values, and a
  default "on" flag would make the identity geometry crop anyway; an operation version is what
  section 8 prescribes for a changed semantics.
- **Apply the user crop (DefaultUserCrop, aspect-ratio crops) as part of the area.** It is defined
  as an adjustable default of the user's crop; folding it into the frame would make the pixels
  outside it unreachable. Exposed instead.
- **LibRaw's `adjust_to_raw_inset_crop()`.** It changes LibRaw's margins, so the visible area, the
  CFA phase and the decode-parity reference move with it; the engine applies the area after
  demosaicing instead, without touching the decoder's view.
- **Read every vendor's crop tags with Lucent's own parsers.** More parsers to maintain; LibRaw's
  readings agree with the manufacturers' sizes in the corpus. Only the RAF edge LibRaw moves is
  restored from the file, and only when LibRaw's value is exactly its adjusted form.
- **Treat a missing area as unsupported.** A border is image data (ADR 0016's edge-band check);
  rendering it is the conservative path of a Limited mode, not a failure.
