# ADR 0016: Camera qualification: mode keys, evidence rules, proposed states and the support table

- **Status:** Accepted (phase 1 of W10: the harness, the expectations and the table; amended by
  ADR 0017 for the camera's image area (check suite 2), on 2026-10-08 by ADR 0018 for the
  camera support database: see "Amendment: support data (ADR 0018)" below, by the closing
  review of W10: see "Amendment: closing review", and on 2026-10-09 by ADR 0020 for the camera
  correctness gate: see "Amendment: W12 camera correctness gate (ADR 0020)", and on 2026-10-10 by the
  closing review of W13: see "Amendment: qualification with lens corrections")
- **Date:** 2026-10-05
- **Workflow:** W10 (Multi-brand qualification), phase 1 of 3

## Context

Blueprint section 3 makes camera compatibility a capability contract: support is attached to
"support files and capture modes, not just brand names", file extensions are hints, and four
user-visible states exist: *Verified* ("exact model and relevant capture mode passed Lucent's
decoding, color, geometry, and export tests"), *Compatible, unverified* (decoder and required
interpretation steps succeed, but Lucent lacks representative quality fixtures), *Limited* (a known
optional feature or profile is unavailable) and *Unsupported* (decoding or a mandatory
interpretation step is unavailable or ambiguous). Section 10's "Camera correctness" gate asks that
each advertised verified model/mode decodes "with correct orientation, dimensions, CFA/linear route,
black/white handling, required corrections, and color interpretation"; it also says to "document a
permissible crop/geometry difference before comparing renderers" and not to "invent passed ... Delta
E ... results". Milestone 5 asks for "an exact model/mode support table".

Before W10 the application derived its support status from the decoded-RAW contract's
interpretation alone (`app::SupportStatus`, `source_item.hpp`): no item was ever Verified. The
corpus is 34 CC0 fixtures from raw.pixls.us (`fixtures/manifest.json`), one daylight test file per
mode, with committed rawpy references for decode parity (ADR 0005) and for rendered colour
(`tests/integration/data/render_reference.json`, ADR 0006).

Facts that shaped the decisions (measured 2026-10-05 with LibRaw 0.22.2, decoded-RAW contract 1.3,
`tests/qualification/tools/file_facts.py` and the web sources cited in
`fixtures/qualification/expectations.json`):

- The decoder's visible area rarely equals the camera's own image size. It is one pixel smaller per
  dimension for the Canon EOS R5 (8191x5463 against Canon's 8192x5464 at full frame, 5087x3391
  against 5088x3392 in 1.6x crop), equal for the Canon S-RAW, the DJI Mini 2 and the Adobe linear
  DNG (and for the Mavic Pro and Mavic 3 against DJI's published size, though not against their DNG
  DefaultCrop), and larger everywhere else: by 6 to 60 pixels for most bodies, by 1.8 % and 2.0 %
  for the Sony A7 IV APS-C crop (4692x3132 against 4608x3072). Most files declare the camera's area themselves: DNG DefaultCrop, the same
  DefaultCrop tags in Sony ARW and Hasselblad 3FR raw IFDs, Fujifilm RAF RawImageCroppedSize,
  Panasonic RW2 sensor borders. Lucent applies none of them (ADR 0006 section 5, ADR 0014).
- Samples above the decoder's white level are common and not a decoder error: every DNG's white
  level equals its WhiteLevel tag, yet the Leica M10 declares 15000 while 5.2 % of its samples reach
  up to 16383; Sony's lossy ARW overshoots 16383 slightly; YCbCr (Canon sRAW, Sony M/S) and linear
  DNG data exceed white after colour conversion. A clip plateau below the white level (which would
  leave clipped highlights short of white) occurs in no fixture.
- The CR3 CMP1 header states the CFA layout and the wavelet levels (0 for RAW, 3 for C-RAW,
  documented by lclevy/canon_cr3), so the R5's CFA and compression can be checked without LibRaw.
  The Nikon Z 6 / Z 8 tags readable without LibRaw do not distinguish lossless, lossy, HE and HE*
  (the raw IFD Compression is 34713 for all; the maker note has no tag 0x0093).
- The develop render's JPEG (q95) differs from its quantised image by 0.6 to 2.2 codes on average
  (single samples up to about 35), more than the W2 path's 1.5 bound in the render tests, because the
  base look keeps more contrast and noise.
- One fixture's full suite takes up to about 30 s (about 6 minutes for the 34 fixtures) and peaks at
  185 MB to 1.4 GB working set (the 61 MP Sony A7R V).

## Decision

1. **Mode key, version 1** (`apps/lucent-qualify/mode_key.hpp`). Qualification evidence attaches
   to a mode key derived from the decoded-RAW contract, never from the extension:
   `mk1|make|model|container|compression|area|<bits>bit|representation|sample|view|subimage`, for
   example
   `mk1|sony|ilce-7m4|tiff|sony-arw-lossless-compressed-ycbcr-m-s-sizes|4624x3080|15bit|linear-rgb|uint16|single-view|primary:0/1`.
   - *make*: canonical maker id from an alias table ("NIKON CORPORATION" -> nikon, "RICOH IMAGING
     COMPANY, LTD." -> pentax unless the model is not a Pentax, "Arashi Vision" -> insta360, "OM
     Digital Solutions" -> om-system); *model*: the file's model slug with a repeated maker name
     removed. LibRaw's normalised model is not used (it calls the X1D II "50-15-Coated5-II-3FR").
   - *compression*: the slug of `DataCompression::name`, whose names are stable by contract; a
     `*` is spelled "star" so that Nikon HE and HE* keep different keys.
   - *area*: the visible area in stored orientation. It separates full frame, APS-C and 1.6x crop,
     Sony L/M/S and drone resolutions, which is what the blueprint's "crop mode" and "DNG at each
     supported resolution" distinctions need.
   - *bits*: a DNG's BitsPerSample, otherwise `ceil(log2(white + 1))` of the decoder's white level
     (`bits_provenance` records which); "f32" for floating-point data.
   - *representation*: `mosaic:<CFA at the visible-area origin>`, `linear-rgb` or `monochrome`;
     *sample*, *view* (single view or dual-fisheye 360) and *subimage* (`role:index/count`) follow
     the contract's enums.
   The key is the decoder's view of the mode on purpose: a decoder upgrade that moves the visible
   area or renames a compression changes the key, and the mode must be qualified again. The
   decoder build and contract version are recorded beside the key as the qualification basis, not
   inside it.

2. **Expectations and the evidence rule** (`fixtures/qualification/expectations.json`, schema
   `lucent.qualification-expectations` v1, `expectations.hpp`). Per fixture: camera identity and
   product, the expected mode key and compression, the rendered size and the camera's image area,
   orientation, representation, mandatory interpretation steps, and the expected outcome (state,
   every check's status, limitations). Every fact has a status and cites sources from a shared
   list with URL, retrieval date and the exact quote. A fact may be `verified` only when at least
   one source is of an independent kind: `manufacturer_spec`, `file_tag` (a value the file
   declares, read by `file_facts.py` with the Python standard library, no LibRaw, no Lucent),
   `independent_decoder` (RawSpeed's `cameras.xml`) or `reviewer_visual` (a dated inspection of
   Lucent's previews). `decoder_reference` (LibRaw, rawpy) and `archive_metadata` (raw.pixls.us
   labels) are recorded but never suffice; the loader rejects a verified fact without an
   independent source. What could not be checked stays `unverified` with a note (for example the
   Nikon and Fujifilm compression modes, the Leica technical data that could not be retrieved, the
   Insta360 product name). The camera image area is the manufacturer's recorded size, or the
   file's declared default crop where the file is a DNG or declares one.

3. **The check suite, version 1** (`qualification.hpp`, `qualify_fixture()`), on Lucent's real code
   paths, each check with a status (pass, fail, not_applicable, not_run) and an evidence strength:

   | Check | Passes when | Strong evidence when |
   |---|---|---|
   | fixture_integrity | size and SHA-256 equal the manifest | always (else the fixture is not qualified) |
   | decode_parity | the parity block (and, for single-channel data, the raw-buffer SHA-256) equals the committed rawpy reference; a file both refuse is refused as Unsupported | always |
   | mode_key | the derived key equals the expectation | the compression fact is verified |
   | subimages | roles are listed; a multi-image file refuses a decode without an explicit subimage; no component frame is complete | always |
   | interpretation | the contract's status is Complete or OptionalOperationsSkipped | always |
   | cfa | representation and CFA at the visible origin equal the expectation; for Bayer data the two green sites have the closer means of the two diagonals (relative difference < 5 % and below the red/blue pair's) | the representation fact is verified |
   | black_white | white above every black level; a DNG's white equals its WhiteLevel; no clip plateau (at least max(64, 1e-6 N) samples at the maximum) below 0.97 white; at most 0.1 % of samples more than 2 % of the range below black. Samples above white are recorded, not failed | always |
   | colour | the W2 render without DNG opcodes passes ADR 0006's outer and per-fixture regression gates against the committed rawpy statistics | Lucent and the reference use the same calibration (decoder matrix or monochrome); DNG camera-profile files compare like for unlike and are weak |
   | dimensions | the develop render's upright size equals the expectation; the camera's image area fits inside it (at most 1 pixel smaller per dimension, at most 3 % larger); the outer 4-pixel bands have 0.5 to 2 times the luminance of the next 16 pixels (single-view frames whose inner band is not dark) | the camera image area is verified |
   | orientation | the decoded orientation equals the expectation, is baked into the pixels and drives the output size | the orientation fact is verified |
   | required_corrections | every mandatory DNG opcode was applied (or applied by the decoder, or skipped by the WarpRectilinear2 rule) and they are exactly the expectation's `dng_opcode:` steps; not applicable without any | always |
   | numerical_validity | W2 scene-linear and display and develop display images are finite; display max(R, G, B) <= 1 | always |
   | export_round_trip | the develop render exported as JPEG (sRGB, q95) and 16-bit uncompressed TIFF (Display P3) with the worker's capture metadata decodes with independent libjpeg / libtiff readers to the quantised samples (TIFF exactly; JPEG mean difference < 3 codes, max 80), carries exactly the profile's ICC bytes and reads back the written metadata with the camera model | always |
   | determinism | a second develop render is bit-identical and its JPEG byte-identical | always |

   The develop render (default `DevelopSettings`, DNG opcodes applied) is the product's export
   path; the W2 render without opcodes is used only where the rawpy reference requires it (ADR
   0014 decision 9). TIFF is written uncompressed because Deflate does not depend on the camera and
   costs about 10 s per 45 MP image; the exporter's own tests cover it. The JPEG bound is wider than
   the render tests' 1.5 for the measured reason above; a row or channel defect differs by tens of
   codes. Measurements per fixture: wall time per phase and, because every fixture runs in its own
   process, that process's peak working set and peak commit, beside the engine's own estimates.

4. **Limitations** (a known optional capability is missing; `limitation_ids()`), detected from data:
   `default_crop_not_applied` (the output exceeds the camera's image area by more than one pixel,
   or a DNG DefaultCropSize is smaller than the visible area), `xtrans_baseline_demosaic` (the
   demosaic route's maturity is Baseline), `dual_pixel_not_processed` (a subimage is a Dual Pixel
   component), `pixel_shift_not_combined`, `dng_enhanced_image_not_offered` (a DNG IFD with
   NewSubFileType 16), `optional_operations_skipped` (interpretation OptionalOperationsSkipped) and
   `non_square_pixels`. Lens-profile corrections are absent for every camera until the Lensfun
   decision (W9); a lens-profile miss is optional by the blueprint ("can leave correction off") and
   is a product-wide gap, stated in the table's preamble, not a per-mode limitation.

5. **State rules, version 1** (`propose_state()`), in this order: *not qualified* when the
   harness could not establish the fixture (missing or altered file, crashed process); *Unsupported*
   when the decoder refuses the mode or any check fails (a failed interpretation check means a
   mandatory step is unavailable); *Limited* when every check passes or does not apply but a
   limitation exists; *Verified* when, in addition, every applicable check passed with strong
   evidence and the expectation exists, its mode key equals the derived one and its identity,
   compression, camera image area, orientation and representation facts are all verified;
   otherwise *Compatible, unverified*, with every evidence gap listed. A mode with several fixtures
   takes the worst of their states. A Verified state holds for the recorded decoder build, contract
   and pipeline versions; the corpus holds one daylight file per mode, and the table says so.

6. **Report and table.** `lucent-qualify` runs the selected fixtures (a tier, or ids) one at a
   time, each in a child process of itself (`--in-process` runs them in one process), and writes
   `lucent.qualification-report` v1 (`report.hpp`): tool and decoder identity, versions (mode key,
   check suite, state rules, contract, pipelines, opcode processing), the inputs with their SHA-256,
   a summary, every fixture result, the support table (one row per mode key) and the coverage
   against `fixtures/qualification/families.json` (schema `lucent.qualification-families` v1: the
   blueprint section 3 families and their targets as mode-key criteria; a target is verified only
   when every matching fixture is, and documented targets without criteria report absent), plus the
   manifest's corpus gaps. `support_table_markdown()` renders `docs/qualification/support-table.md`.
   Exit status 0 when every fixture produced exactly its expected outcome, 3 on a difference, 2 when
   a fixture could not be qualified, 1 for usage errors.

7. **CI.** `lucent_qualification_fixture_tests` (label `qualification`, holding the full-resolution
   render lock) runs `qualify_fixture()` for every cached fixture and asserts the expected state,
   every check status, the limitations and the mode key; smoke fixtures always, the qualification
   tier with `LUCENT_TEST_FIXTURE_TIER=all`. `lucent_qualification_tests` covers the mode key, the
   expectations schema and evidence rule, the state rules, family matching and the report writers
   on synthetic data, and that the committed expectations cover the manifest exactly and agree with
   the state rules. Adding a camera follows `docs/qualification/README.md`.

## Consequences

- Run of 2026-10-05 over all 34 fixtures (every outcome equal to its expectation): **Verified 4**
  (Canon EOS R5 RAW and C-RAW full frame, R5 RAW 1.6x crop, EOS 5D Mark IV S-RAW), **Compatible,
  unverified 2** (DJI Mini 2 DNG and the Adobe-converted 5D Mark III linear DNG: every check passes,
  but their colour comparison is not like for like), **Limited 25** (24 for the default crop, the two
  X-Trans files also for the baseline demosaic, the R5 Dual Pixel file for Dual Pixel data, the
  Lightroom DNG for its Enhanced image), **Unsupported 3** (Nikon Z 8 HE and HE*, which LibRaw cannot
  decode, and the Insta360 dual-fisheye source, which needs stitching). No check failed outside those
  three. The table and the report are in `docs/qualification/`.
- The default crop is the single largest gap: applying the camera's declared image area (DNG
  DefaultCrop, the ARW/3FR crop tags, RAF and RW2 crops) and a per-model area where the file
  declares none would move most Limited modes to Verified or Compatible, unverified. It needs a new
  geometry stage in the engine (`src/engine`, outside this workflow) and contract fields for the
  non-DNG crops (`src/raw`).
- Verified evidence is thin by construction: one daylight shot at base ISO per mode, no lens
  variety, no firmware variety. Colour "like for like" proves that Lucent reproduces LibRaw's
  calibration, not that the calibration is right; DNG profile colour has no independent reference
  until the DNG SDK or another reference renderer is available.
- The application still shows "Supported/Limited/Unsupported" from the interpretation alone.
  Consuming the report (Verified by mode key, Limited with reasons) is phase 2 and belongs to the
  application owners (`src/application`, `apps/lucent`); it must match on the mode key and the
  qualification basis (decoder build and contract), never on make or extension.
- Maintenance: a LibRaw upgrade, a contract change or an engine change that alters a check fails the
  GTest wrapper until the expectation is reviewed and updated together with the table. Web sources
  can move; each cites its retrieval date and quote. RawSpeed's `cameras.xml` is CC BY-SA 3.0: only
  facts are cited, the file is not copied.
- Revisit when the default crop is implemented, when Lensfun lands, when fixtures of the absent
  targets (DJI Mini 4 Pro, Insta360 Ace series, Fujifilm pixel shift, R5 full-frame Dual Pixel) or a
  scene-diverse corpus are acquired, or when a non-LibRaw decoder is added.

## Alternatives considered

- **Key by brand, model and extension.** Rejected by the blueprint and by the data: one ARW body has
  six modes with three representations and four sizes in the corpus.
- **The decoder's normalised make/model.** Rejected: LibRaw names the X1D II "50-15-Coated5-II-3FR"
  and the Mavic 3 camera "L2D-20c-FFF"; the file's own strings are what a reviewer can check.
- **Decoder version inside the key.** Rejected: every LibRaw patch release would invalidate every
  key even when nothing changed; the version is recorded as the basis, and a real change shows up in
  the key's components or in a failing check.
- **Dimensions must equal the manufacturer's size exactly.** Rejected: LibRaw's area is a legitimate
  decoder convention (RawSpeed's differs again), the extra border is image data (the edge-band check
  measures it), and failing would call 28 decodable modes Unsupported. A documented permissible
  difference (1 pixel short, 3 % long) with a Limited state for an uncropped border is honest about
  what the user gets.
- **Any sample above white fails.** Tried first; 13 of 32 decodable fixtures failed for reasons
  that are properties of the files (declared white levels, codec overshoot, colour conversion).
  Replaced by the checks that detect wrong levels: the DNG tag agreement, the clip plateau and the
  black floor.
- **Treat DNG files' colour as strong evidence because the gross gates pass.** Rejected: "never mark
  Verified on weak evidence"; Lucent's DNG profile path and LibRaw's D65 matrix differ by design
  (ADR 0006), so the comparison bounds gross errors only.
- **Derive expectations from Lucent's own output.** Rejected as circular: facts come from
  independent sources; only the expected outcomes are recorded from a reviewed run, and those
  predictions were written before the run that confirmed them.
- **All fixtures in one process.** Rejected as the default: peak memory per fixture would be the
  process's running maximum, and a decoder crash would end the run; `--in-process` remains for
  debugging and the GTest wrapper.
- **Make lens-profile absence a per-mode limitation.** Rejected for now: it applies to every mode
  equally until Lensfun is decided, so it is stated once for the product rather than repeated in
  every row.

## Amendment: support data (ADR 0018), 2026-10-08

Phase 3 of W10 turns the report into the camera support database the application uses
([ADR 0018](0018-camera-support-data.md)). It changes three things here:

- **Decision 1.** The mode key's definition moved unchanged to `lucent_assets`
  (`src/assets/include/lucent/assets/mode_key.hpp`); `apps/lucent-qualify/mode_key.hpp` forwards to
  it. The application derives the same key when it probes a file at import.
- **Decision 6.** Each fixture result records `lookup_key`, the mode key of the file's probe (stage
  AfterOpen), which is the key the application looks the mode up under; each support-table row
  lists its fixtures' probe keys as `lookup_keys`, and the mode-key check records a probe key that
  differs from the decoded key in its details. The probe key equals the decoded key for 33 of the
  34 fixtures; for the Canon EOS 5D Mark IV S-RAW the probe's white level gives 15 bits and the
  decode's 14. The committed report was regenerated on 2026-10-08T21:47Z with these fields and the
  same outcomes for all 34 fixtures (Verified 14, Compatible, unverified 11, Limited 6,
  Unsupported 3); apart from the new fields, only its time and measurements changed.
- **Consequences, "the application still shows ...".** No longer: `lucent-qualify --from-report
  <report> --export-support assets/camera-support --asset-version <version>` exports the database
  (refusing partial runs and runs with expectation mismatches), the application combines its
  answer for the probe's key with the live interpretation into the four states, and a Verified
  state holds only while the database's basis (decoder, decoded-RAW contract, mode key and
  pipeline versions) is the running build's. The match is on the mode key and the basis, never on
  make or extension, as this ADR required.

New tests: `SupportExport.*` and `CommittedData.TheCameraSupportAssetIsTheExportOfTheReport`
(`lucent_qualification_tests`), and `QualificationFixture.ProbeKeyFindsTheQualifiedState` (every
cached fixture of any tier, in `lucent_qualification_fixture_tests`).

## Amendment: closing review (2026-10-08)

- **What colour and decode evidence is (QUA-03).** Decision 3 grades decode parity and like-for-like
  colour Strong, although their reference is built on the same decoder (rawpy, LibRaw 0.22.1): they
  show that Lucent reads the data and reproduces LibRaw's camera calibration, not that the
  calibration matches an independent measurement, as the Consequences already said. The evidence
  rule of decision 2 (for facts) is unchanged and the state rules are not weakened: Verified still
  needs every expected fact from an independent source. What changed is what the product claims:
  the support table, `docs/qualification/README.md` and the Verified meaning in the shell now say
  that colour is verified as agreement with the decoder's calibration, and the README no longer
  says that decode parity always includes the raw-buffer hash (full-colour data has none; its
  pixels are compared through the colour check). An independent colour reference (chart patches of
  a corpus file that shows a colour checker, such as the 5D Mark IV RAW's SpyderCHECKR, against the
  chart's published values, or a DNG SDK rendering) is deferred to the release gates (W12).
- **Regression net (QUA-07).** The GTest wrapper qualifies every cached fixture whose expected state
  is Verified by default, not only the smoke tier, and asserts where the camera's area was placed:
  `expectations.json` records `dimensions.image_area_applied` ([x, y, width, height] in the upright
  image, or null for the whole image) for every rendered fixture, an outcome recorded from the
  reviewed run whose origins equal the crop origins the files' tags declare (each fixture's note);
  the harness reports a different placement as an expectation mismatch. A rotated corpus file
  (orientation 6 or 8) per image-area source is still missing: every corpus file has orientation 1,
  and rotated placement is covered by the engine's synthetic tests only (deferred with the corpus
  expansion, W12).
- **Provenance (QUA-06).** The report and the database name the build's commit; it now carries
  "-dirty" when the tree had uncommitted changes at configure time, and the export refuses such a
  report unless `--allow-dirty-build` (ADR 0018 amendment). The report of 2026-10-08 that was
  committed with commit 3a836ac's name came from an uncommitted tree; the committed report is now
  the run of a fresh build of commit 0161428 (no "-dirty" marker), from which database 2.0.1 was
  exported.
- **Probe facts (QUA-08).** Each fixture result records `lookup_variant` (the probe's image area and
  DNG opcodes) and each support-table row `lookup_variants`; the database's Verified entries hold
  only for files with those facts (ADR 0018 amendment).
- Run of the review fixes over all 34 fixtures: every outcome equal to its expectation, the same
  states as before (Verified 14, Compatible, unverified 11, Limited 6, Unsupported 3).

## Amendment: W12 camera correctness gate (ADR 0020), 2026-10-09

[ADR 0020](0020-camera-correctness-gate.md) answers the two gaps the closing review deferred:

- **Colour against an independent reference (QUA-03).** Check suite 3 adds `colour_reference`
  (decision 3's table gains a row): for a fixture whose photograph shows a colour chart
  (`fixtures/qualification/charts.json`: the 5D Mark IV RAW and S-RAW with a Datacolor
  SpyderCHECKR, the X-T4 pair with an X-Rite ColorChecker Classic Mini), the develop path's as-shot
  scene-linear patch colours, balanced on the chart's own grey, must lie within CIEDE2000 mean 4.5,
  90th percentile 7.5 and maximum 12.5 of the manufacturer's published values; evidence
  "independent". Every other fixture's `colour_reference` is `not_applicable`. Measured: mean 2.84 to
  2.99, within 0.01 of LibRaw's own rendering measured with independent code
  (`research/tools/chart_reference.py`).
- **State rules 2** (decision 5): evidence "independent" counts as strong; the rules are unchanged,
  so a chart fixture needs `colour_reference` to pass for Verified, and every other mode keeps its
  like-for-like (LibRaw) or gross-error colour evidence. Each fixture result and support-table row
  records `colour_evidence` and the support table shows it per mode; the decision not to require
  chart evidence for every Verified mode, and its reasons, are in ADR 0020.
- **`black_white`** (decision 3) evaluates the camera's image area when the file declares one (padding
  columns outside a DNG DefaultCrop are not image data).
- **Rotated fixtures (QUA-07).** Six CC0 files with EXIF orientation 6 or 8 cover every image-area
  source (decoder inset crop twice, DNG DefaultCrop twice, the DNG default whole image with mandatory
  opcodes, and an unknown area); their expectations pin the upright placement derived from the
  files' own tags. Expectations give sizes in upright pixels (`expectations.hpp`).
- The corpus has 40 fixtures; the committed report, support table and camera support database are
  regenerated from a clean build of this change.

## Amendment: qualification with lens corrections (W13 closure, 2026-10-10) - APL-01

Since W13 Lucent's exports correct exactly matched lenses by default (ADR 0023), but every develop
render of `lucent-qualify` ran with the engine's default options, which hold no lens database: a mode
would have been Verified from renders Lucent no longer makes (13 renderable exact matches in the
corpus), and the report generator still printed that lens corrections were "not available for any
camera yet".

- **Decision: the qualification renders lenses as the exports do.** `lucent-qualify` loads the
  bundled lens database next to it (`assets::load_lens_database()`: the build's pin, the asset's
  hashes) in every process that renders, and refuses to run without it (exit 1, the reason on
  stderr): a qualification without it would certify renders Lucent does not make. The develop
  renders (`develop_1`, the determinism render, their memory estimate) and the colour reference's
  stage one and two use it (`QualifyOptions::lens_database`). The alternative, qualifying with
  corrections off and stating that Verified excludes them, was rejected: a Verified state would then
  not cover the default export, which is what the photographer gets.
- **The colour reference is unaffected by construction.** It measures operation 1's colour transform
  on stage one's camera samples inside the chart's boxes (ADR 0020), before any lens correction, where
  the boxes locate the patches; a lens correction moves samples and scales them by a
  channel-independent gain, so it changes no colour transform. Its renders carry the database only so
  that every develop render of a fixture plans the same correction.
- **What each fixture records.** The result's `lens_correction`: the database, the match ("exact",
  "fuzzy", "none"), the profile and each correction's outcome; the support table gains the section
  "Lens corrections" with them per fixture, and its introduction says what a Verified state covers.
- **The basis names the database** (`versions.lens_database` of the report, `QualificationBasis::
  lens_database` of the camera support database, schema 3; ADR 0018's amendment). It is the bundled
  asset's pinned manifest (`assets::lens_database_basis()`), so another database is another basis.
- **Expectations unchanged.** A correction keeps the image's size and frame (the corrected canvas is
  the uncorrected image's size, the camera's image area the same pixel rectangle of it, ADR 0023
  decision 7), and the checks compare sizes, orientation, levels, colour and the export's round trip,
  not pixel hashes of a reference: the expected states and check statuses of all 40 fixtures hold.
  The committed report, support table and camera support database are regenerated from a clean build
  of this change.
