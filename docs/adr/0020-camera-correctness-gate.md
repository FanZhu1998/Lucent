# ADR 0020: Camera correctness gate: an independent colour reference from colour charts, and rotated fixtures for every image-area source

- **Status:** Accepted
- **Date:** 2026-10-09
- **Workflow:** W12 (Release hardening), camera correctness
- **Amends:** ADR 0016 decision 3 (check suite 3: the check `colour_reference`; `black_white` on the
  camera's image area) and decision 5 (state rules 2: evidence "independent"), see ADR 0016,
  "Amendment: W12 camera correctness gate"

## Context

Blueprint section 10's *Camera correctness* gate asks that each advertised verified model and mode
decodes "with correct orientation, dimensions, CFA/linear route, black/white handling, required
corrections, and color interpretation", and section 10 adds: "Set numerical image-quality
thresholds using calibration data before final qualification; do not invent passed PSNR, Delta E,
speed, or user-preference results." Section 5 fixes the colour contract: scene-linear Rec.2020 / D65
working space, "an explicit camera-to-reference transform and adaptation", and "Little CMS handles
ICC conversions; it does not invent an accurate input camera profile."

The W10 qualification (ADR 0016) proposes 14 Verified modes, but its colour evidence is agreement
with LibRaw: the rawpy reference uses the same decoder and the same camera matrix, so like-for-like
colour shows that Lucent reproduces LibRaw's calibration, not that the calibration is right (review
finding QUA-03). And every corpus file had EXIF orientation 1: placement of the camera's image area
after a rotation was covered by synthetic engine tests only (QUA-07).

Facts established for this decision (2026-10-09; LibRaw 0.22.2 in Lucent, rawpy 0.27.1 with LibRaw
0.22.1, colour-science 0.4.7):

- **Charts in the corpus.** Every fixture's preview was inspected. Four show a colour chart with
  published reference data: `canon-eos-5d-mark-iv-cr2-raw` and `canon-eos-5d-mark-iv-cr2-sraw` (a
  Datacolor SpyderCHECKR, 48 patches, open on a snow-covered wall in direct sun) and
  `fujifilm-x-t4-raf-lossless` and `fujifilm-x-t4-raf-compressed` (an X-Rite ColorChecker Classic
  Mini held portrait on a tree trunk in forest shade). The other 30 W10 fixtures show no chart (a
  Rubik's cube in the Pentax K-3 Mark III frame has no published colours); neither do the six W12
  rotated fixtures.
- **Reference data.** Datacolor's *SpyderCHECKR Color Data* (2014, retrieved from the Internet
  Archive's 2016 capture: the original URL now redirects) gives L*a*b* per patch without naming the
  illuminant; its own sRGB column is reproduced from those values only with a D50 white (Bradford to
  D65: mean 0.77, largest 4.3 codes over 47 legible patches; with a D65 white: mean 1.83, largest
  25.0), so the data is D50, the ICC convention. X-Rite publishes CGATS reference files for the
  ColorChecker Classic before and after its November 2014 reformulation (i1Pro 2, M0); the Mini
  carries the Classic's 24 colours (X-Rite: "1/8th the size of the ColorChecker Classic Standard";
  Calibrite, its successor: "A standard 24-patch ColorChecker Classic target in a handy credit card
  size"). URLs, retrieval dates, document hashes and quotes are in
  `fixtures/qualification/charts.json`.
- **Measurement.** Each patch was located with `research/tools/chart_reference.py locate` (four
  corner centres per patch grid, edges refined on the image, the grid fitted again; the sample box is
  the central half of the patch side) and every box reviewed on an overlay. Measured on Lucent's
  develop path and, independently, on LibRaw's own rendering (rawpy: as-shot white balance, AHD or
  LibRaw's X-Trans route, LibRaw's highlight clip, LibRaw's camera matrix applied in floating point),
  CIEDE2000 against the published values:

  | Fixture | Develop path, chart balanced: mean / p90 / max | Develop path, as shot | LibRaw's rendering, chart balanced | LibRaw's rendering, as shot |
  |---|---|---|---|---|
  | 5D Mark IV RAW (48 patches) | 2.90 / 4.68 / 5.88 | 2.87 / 4.68 / 5.94 | 2.90 / 4.68 / 5.89 | 2.87 / 4.68 / 5.94 |
  | 5D Mark IV S-RAW (48) | 2.99 / 4.83 / 5.86 | 3.01 / 4.78 / 5.91 | 2.99 / 4.83 / 5.86 | 3.01 / 4.78 / 5.91 |
  | X-T4 lossless (24) | 2.84 / 4.82 / 7.96 | 7.82 / 13.82 / 16.53 | 2.85 / 4.85 / 7.99 | 7.78 / 13.77 / 16.45 |
  | X-T4 compressed (24) | 2.88 / 4.82 / 8.14 | 6.97 / 12.75 / 14.34 | 2.89 / 4.86 / 8.17 | 6.94 / 12.70 / 14.24 |

  The C++ check and the Python tool (colour-science on Lucent's documented W2 scene-linear dump)
  agree per patch within 6e-5. The X-T4's as-shot error is the capture's white balance, not
  colour interpretation: its neutrals have b* between -6 and -17 in both renderings (forest-shade
  light that the camera's white balance did not neutralise); balanced on the chart's own grey, the same
  data is at 2.84 to 2.89. The X-T4 chart's edition cannot be read (the date is on its back); X-Rite's
  pre-2014 values change its chart-balanced mean by at most 0.07. The worst patches are the saturated ones
  that a 3 x 3 matrix reproduces worst (ColorChecker cyan F3 about 8; SpyderCHECKR apple green,
  sunflower, evergreen 5.4 to 5.9).
- **Literature on matrix-profile accuracy.** Least-squares 3 x 3 characterisation of 37 simulated
  cameras over 1995 reflectances and 102 illuminants reaches a mean CIEDE2000 of 1.88 (Vazquez-Corral,
  Connah and Bertalmío, "Perceptual Color Characterization of Cameras", *Sensors* 14(12):23205-23229,
  2014, doi:10.3390/s141223205, Table 2): an ideal, noise-free, fitted case. A profiling tool's
  documentation: "A modern camera can usually match colors quite well with just a matrix, but there's
  still often up to 3-4 DE on some colors on a typical target" and "if you do see large errors, such
  as 10 DE or more, it's likely that there is some wrong with your input data, such as poor lighting
  of the test target, glare, bad references values" (A. Torger, DCamProf documentation,
  https://www.ludd.ltu.se/~torger/dcamprof.html, retrieved 2026-10-09). Imatest's documentation: "For
  now I would characterize the values of ΔC00 mean = 3.43 and maximum = 5.84 for the Micro Four-Thirds
  camera ... as very good" (CIEDE2000 chroma of a camera's rendered output;
  https://docs.imatest.com/docs/colorcheck, retrieved 2026-10-09). LibRaw's matrices are generic
  (Adobe's per-model ColorMatrix), not fitted to the photographed chart, and field captures add
  flare, uneven light and chart-to-chart variation (X-Rite calls its files "a generic, averaged
  colorimetric description"), so a correct pipeline is expected above the fitted ideal: the
  measured 2.8 to 3.0 means agree with this.
- **Sensitivity** (`research/tools/chart_reference.py` on LibRaw's camera-RGB patch means, so
  independent of Lucent; chart-balanced mean / p90 / max, 5D Mark IV RAW and X-T4 lossless):
  correct 2.90 / 4.68 / 5.89 and 2.85 / 4.85 / 7.99; red and blue exchanged 20.2 / 45.6 / 58.4 and
  24.2 / 50.7 / 56.1; no camera matrix (camera RGB taken as sRGB) 7.27 / 13.40 / 16.57 and 6.73 /
  11.14 / 11.84; the other test camera's matrix 4.59 / 8.70 / 10.07 and 5.20 / 7.85 / 13.52; white
  balance applied twice 4.95 / 9.61 / 23.49 and 22.3 / 43.7 / 82.6; white balance missing 3.69 / 7.53
  / 9.21 and 6.55 / 11.83 / 15.32; a display curve (1/2.2) on linear data 11.6 / 16.4 / 18.0 and 12.8
  / 16.8 / 17.2; black level 1 % of white too low 3.69 / 5.79 / 6.90 and 3.18 / 5.11 / 8.41, too high
  3.43 / 5.64 / 9.56 and 3.05 / 5.66 / 7.74; the D50 to D65 adaptation missing: unchanged (the chart
  balance removes it). As shot, the X-T4 scene makes the missing adaptation look better (4.81 against
  the correct 7.77), so the as-shot statistics cannot be gated for a capture whose white balance does
  not match the chart's light.
- **Rotated files** (2026-10-09, the `raw.pixls.us` index scanned through each CC0 sample's EXIF
  dump: 67 of the 1744 CC0 samples declare another orientation than 1, most of them 6 or 8; 96 dumps
  show no orientation tag). For each image-area source Lucent uses there are CC0 samples
  with orientation 6 or 8; LibRaw (rawpy) reproduces Lucent's parity block exactly for all six chosen,
  and LibRaw's own rotated rendering agrees with Lucent's by regions (CIEDE2000 mean 0.005 to 0.6).
  No DJI file in the archive is rotated (drones write orientation 1); Android phone DNGs carry
  OpcodeList2 gain maps and are often rotated. Older Panasonic RW2 and Leica RWL files (DMC-TZ60,
  DMC-FZ70, Leica C (Typ 112), all rotated) are refused by Lucent as truncated ("strip data ends at
  byte 4294967295") although LibRaw decodes them (see Consequences).

## Decision

1. **The check `colour_reference`** (`apps/lucent-qualify/colour_reference.hpp`, check suite 3)
   applies to every fixture listed in `fixtures/qualification/charts.json` and is `not_applicable`
   for every other fixture, decoded or not.
   - *Measured path:* the develop path's export render at its as-shot defaults (`DevelopSettings{}`:
     as-shot white balance, no look, no curves, geometry version 2), taken apart into stage one (kind
     Export) and stage two's report so that operation 1's scene-linear values are available before the
     base look: per patch, the mean of `min(s * d, clip)` over the box in white-balanced camera RGB,
     mapped with stage two's own `gain * M_ws * camera_to_xyz`. The frame (the camera's image area in
     the upright image, from stage two's report) must equal the chart definition's frame, size and
     origin; otherwise the check fails: the boxes would no longer locate the patches.
   - *Conversion and normalisation:* Rec.2020 / D65 to CIE XYZ; *chart balanced* (gated): one
     Bradford von Kries transform mapping the measured normalisation patch exactly onto its reference
     XYZ (exposure normalisation and adaptation of the scene's white, as the chart records it, to the
     reference white D50); *as shot* (reported): Bradford from the working white D65 to D50, then one
     scale giving the normalisation patch its reference Y. Normalisation patches: a light grey of
     L* about 66 (SpyderCHECKR 3E "40% Gray", ColorChecker C4 "neutral 6.5"). CIELAB relative to
     D50 (Y = 1, CIE 1931 2 degree xy (0.3457, 0.3585)); CIEDE2000 with kL = kC = kH = 1 (the
     implementation the colour check already uses, Sharma, Wu and Dalal 2005). Statistics over every
     measured patch: mean, 90th percentile (linear interpolation, numpy's default) and maximum (the
     normalisation patch, mapped exactly, counts with 0 in the chart-balanced statistics). Per-patch
     values are in each report's check details and in `chart_reference.py`'s JSON output.
   - *Clipping:* a patch with more than 0.1 % of its box's samples at or above the white-balanced
     clip level is excluded and reported; the chart is not measurable (the check `not_run`, an
     evidence gap) when its normalisation patch is excluded or fewer than 75 % of its patches remain.
     No corpus patch is excluded (the X-T4 lossless white patch has 0.003 % clipped samples on the
     develop path, 0.008 % in LibRaw's rendering).
   - *Gate:* chart-balanced mean <= **4.5**, 90th percentile <= **7.5**, maximum <= **12.5** CIEDE2000.
     Rule: 1.5 times the largest value LibRaw's own rendering reaches on any corpus chart (2.99, 4.86,
     8.17), rounded up to half a unit. The margin absorbs other captures' conditions (flare, uneven
     light, chart editions); the limits stay below where the simulated gross errors land (no or another
     camera's matrix, channels exchanged, white balance twice, a display curve on linear data) and
     inside the literature's range for a correct matrix profile under field conditions. As-shot
     statistics are reported and never gated (the X-T4 capture's white balance would make any as-shot
     limit either meaningless or a failure of correct data).
   - *Evidence* "independent": the reference depends on neither LibRaw nor Lucent. The check's details
     record the chart, its source and retrieval date, the develop path's calibration path, gain
     and clip level, per-patch reference, as-shot and chart-balanced L*a*b* and CIEDE2000,
     exclusions, the alternate reference's statistics, and the independent rendering's statistics from
     charts.json for comparison.
   - A chart outside the gate fails the check, and a failed check makes the mode Unsupported (state
     rules unchanged): a colour interpretation outside matrix-profile accuracy is not a conservative
     path.

2. **Chart data** (`fixtures/qualification/charts.json`, schema `lucent.qualification-charts` v1,
   loader `charts.hpp`): published reference sets (D50 and the CIE 1931 2 degree observer only; patch
   L*a*b*; URL, retrieval date, document SHA-256, the exact quote and how the illuminant was
   established) and, per chart fixture, the reference, alternate references (sensitivity only), the
   frame (size and origin), the normalisation patch, one sample box per patch, how the boxes were
   located, the chart's identification and scene, and the statistics of the independent rendering
   (`research/tools/chart_reference.py measure --write-charts`). The loader refuses anything the
   measurement could get wrong silently (another illuminant or observer, a box leaving the frame, a
   reference patch without a box, an unknown patch or reference, L* outside 0 to 100).

3. **The independent reference** (`research/tools/chart_reference.py`): `measure` renders each chart
   fixture with rawpy (LibRaw's demosaic, white balance, highlight clip and camera matrix, the matrix
   applied as LibRaw's `cam_xyz_coeff()` derives it, in floating point so that bright bluish whites
   are not clipped by LibRaw's 16-bit output) and with colour-science's CIELAB, Bradford and CIEDE2000
   implementations; `--lucent-render` adds Lucent's documented W2 scene-linear dump as a cross-check of
   the C++ check. `locate` produces the sample boxes of a new chart from its corner patches. Neither
   shares code with Lucent's colour stages: the comparison is independent of Lucent's code, and its
   reference values are independent of LibRaw.

4. **State rules 2.** Evidence gains the value "independent" (stronger than "strong"; it satisfies
   every rule that asks for strong evidence). The rules are otherwise those of version 1, so:
   - a chart fixture is Verified only when `colour_reference` passes as well (it is an applicable
     check); one that fails is Unsupported; one whose chart is not measurable is Compatible, unverified;
   - every other fixture keeps its W10 colour evidence: like-for-like agreement with LibRaw's
     calibration (strong, decoder matrix and monochrome) or gross errors only (weak, DNG camera
     profiles). Requiring the independent check for Verified would downgrade 14 of the 16 Verified
     modes for lack of a chart in their single test file, not for any colour fault; instead each
     fixture result and support-table row records `colour_evidence` ("independent_chart",
     "decoder_calibration_agreement", "monochrome", "gross_errors_only" or "none"), the support table
     shows it per mode and the evidence text names independent checks ("14 of 14 checks strong;
     independent: colour_reference"), which the camera support database carries per entry.

5. **`black_white` on the image data** (check suite 3): the level statistics are taken over the
   camera's image area inside the visible area when the file declares one, else over the visible
   area; the green-site phase stays relative to the visible origin. A file's border outside its
   declared area may be padding: the bq Aquaris U Plus DNG's last eight columns read about 5 against
   a black level of 64 and failed the black-floor test (0.17 % of samples far below black) although
   none of them is in its DefaultCrop (inside it: 0.0009 %).

6. **Rotated fixtures** (manifest tier qualification; cited expectations; upright sizes, so the
   manufacturers' landscape sizes appear with width and height exchanged):

   | Fixture | Orientation | Image-area source | Placement checked (upright `image_area_applied`) | State |
   |---|---|---|---|---|
   | `canon-eos-350d-cr2` | 8 | decoder inset crop (Canon SensorInfo) | (6, 8) 2303 x 3456; orientation 6 would give (5, 10) | Verified |
   | `olympus-xz-10-orf` | 6 | decoder inset crop (ImageProcessing crop) | (0, 8) 2976 x 3968; orientation 8 would give (14, 8) | Verified |
   | `ricoh-gxr-s10-dng` | 6 | DNG DefaultCrop, no ActiveArea | (6, 12) 2736 x 3648 (unrotated (12, 6)) | Compatible, unverified (DNG colour) |
   | `bq-aquaris-u-plus-dng` | 8 | DNG DefaultCrop, OpcodeList2 GainMap (optional) | (8, 8) 3464 x 4624 | Compatible, unverified (DNG colour) |
   | `oneplus-one-dng` | 6 | DNG default (whole ActiveArea), mandatory OpcodeList2 GainMap | whole image 3120 x 4208 | Compatible, unverified (DNG colour) |
   | `sony-dslr-a450-arw` | 6 | unknown (no crop declaration) | whole image 3072 x 4608 | Limited (default crop not applied) |

   The expected placements are derived from the files' own tags read without LibRaw
   (`tests/qualification/tools/file_facts.py`, which now also reads Canon's SensorInfo and Olympus'
   ImageProcessing crop) and RawSpeed's visible areas, and the notes in `expectations.json` give
   each derivation; the 350D and XZ-10 margins differ between opposite sides, so a rotation in the
   wrong direction or an unrotated area cannot reproduce them. Each fixture also has a rawpy parity
   reference and rawpy render statistics (`tests/integration/data/render_reference.json`), whose
   region comparison checks the rotation's direction against LibRaw's own reading of the
   orientation.

7. **How a future camera gets chart evidence.** (a) A CC0 file of exactly that mode showing a chart
   with published values (a ColorChecker Classic or SpyderCHECKR, evenly lit, without glare, no patch
   clipped after white balance, at least a few hundred pixels per patch); (b) its reference set in
   `charts.json` if new, with URL, retrieval date, hash, quote and the established illuminant; (c)
   `lucent-render <file> --out <dir> --legacy-neutral-v0 --dump-linear <tiff>` (the camera-area frame),
   corner centres read off a preview, `chart_reference.py locate`, review of its overlay, the
   fixture's entry with the frame of its expectation (rendered size and `image_area_applied` origin);
   (d) `chart_reference.py measure --ids <id> --write-charts`; (e) the expectation's
   `colour_reference` set to the outcome the run gives. A chart whose independent rendering exceeds
   the gate is investigated (lighting, glare, edition, reference data) and not admitted by raising
   the gate; the gate itself changes only through this ADR, with new measurements.

## Consequences

- Run of 2026-10-09 over all 40 fixtures (check suite 3, state rules 2; 9.5 minutes; every outcome
  equal to its expectation): **Verified 16, Compatible, unverified 14, Limited 7, Unsupported 3**. The
  four chart fixtures pass `colour_reference` (1 to 2.7 s each); the 5D Mark IV RAW and S-RAW stay
  Verified, now with independently verified colour; the X-T4 pair stays Limited (X-Trans baseline
  demosaic) with independent colour evidence. No existing fixture's state, check status, evidence,
  evidence gap, limitation or mode key changed (compared with the committed W10 report). The six
  rotated fixtures add Verified 2, Compatible, unverified 3 and Limited 1. Colour evidence over the 40
  fixtures: independent chart 4, agreement with LibRaw's calibration 21, monochrome 2, gross errors
  only (DNG camera profiles) 11, none 2 (the Nikon HE files LibRaw refuses).
- **No rotation defect was found**: every rotated file renders upright, its frame is placed where the
  file's own tags put it, and LibRaw's independent rotation agrees. **A decoder defect was found**
  while searching (not fixed here: `src/raw` is outside this workflow): Lucent's pre-decode
  truncation check refuses older Panasonic RW2 and Leica RWL files whose IFD 0 StripOffsets is the
  placeholder 4294967295 with StripByteCounts 0 (the raw data sits at Panasonic tag 0x0118,
  RawDataOffset), reporting `raw.libraw.truncated` "the file is truncated or damaged (ifd0: strip
  data ends at byte 4294967295 ...)", although LibRaw decodes them (raw.pixls.us samples 5535 DMC-TZ60,
  3755 DMC-FZ70, 4963 Leica C (Typ 112)). Such a camera mode is shown as Unsupported for a file it
  could develop; the check should ignore strip tags that LibRaw does not use for the format (or
  validate the RW2 RawDataOffset instead).
- Independent colour evidence exists for two cameras only (Canon EOS 5D Mark IV RAW and S-RAW,
  Fujifilm X-T4 lossless and compressed). Every other mode's colour, including all Sony, Nikon,
  Panasonic, OM System, Pentax, Leica, Hasselblad, DJI, Insta360 and the new rotated modes, rests on
  LibRaw agreement or, for DNG camera profiles (DJI, Hasselblad L2D-20c, Leica M10, Adobe linear DNG,
  Lightroom DNG, Ricoh GXR, bq, OnePlus), on gross-error bounds only; DNG profile colour has no
  independent reference at all, and monochrome modes have no colour to verify. Chart captures of
  those modes (CC0, or shot for the project) are the next evidence to acquire; the support table's
  "Colour evidence" column shows the gap per mode.
- The chart-balanced gate verifies what the calibration does to colour up to a white point and an
  exposure; it does not see a white-point adaptation error or a black level 1 % of white off (both
  simulated: within the gate). Those remain covered by decode parity, `black_white` and the
  like-for-like colour check, and the as-shot statistics are reported beside every chart result.
- A single chart photograph per mode is thin evidence: one light, one exposure, one chart. The gate's
  margin is a judgement on four captures of two charts; revisit it when more chart fixtures exist
  (re-derive with the same rule) or if a correct rendering of a well-lit chart exceeds it.
- The committed qualification report, support table and camera support database must be regenerated
  from a clean build of this change (check suite 3, state rules 2, six new modes, new evidence text;
  the database's content changes, so its version rises, 2.1.0 for added modes): until then
  `CommittedData.PublishedReportAgreesWithTheExpectations` and
  `QualificationFixture.ProbeKeyFindsTheQualifiedState` (six new fixtures) fail, by design: the
  committed data must not contradict the expectations. The tests read a regenerated report and asset
  folder instead when `LUCENT_QUALIFICATION_REPORT` and `LUCENT_CAMERA_SUPPORT_DIR` name them; with this
  change's own regenerated outputs every qualification test passes (unit tests 38 of 38; with
  `LUCENT_TEST_FIXTURE_TIER=all`, 84 of 84 fixture tests: every fixture's outcome, probe key and chart).
- Maintenance: a LibRaw upgrade that changes a camera matrix, or an engine change in white balance,
  demosaicing or camera colour, moves the chart statistics; the check passes as long as the gate holds,
  and `ColourReferenceFixture` (GTest) additionally requires Lucent's statistics to stay within 0.25
  (mean) and 0.5 (p90, max) of the recorded independent rendering, so a drift is reviewed. The
  Datacolor document survives only in the Internet Archive; its hash is recorded.

## Alternatives considered

- **Gate the as-shot statistics.** Rejected by the data: the X-T4 capture's white balance leaves
  neutrals at b* -6 to -17 in both renderings; an as-shot limit would either fail correct data or be
  so wide (about 17) that it catches nothing a chart-balanced limit does not.
- **Compare Lucent with LibRaw on the chart (pass if within x of rawpy).** That is the W10 evidence
  again; it is recorded as a regression bound in the GTest wrapper, never as the gate.
- **Normalise on several greys by least squares, or white-balance on the white patch.** The white
  patch is the first to clip and the darkest greys carry flare; one light grey of L* about 66 is the
  usual practice (Imatest derives its luminance correction from the grey patches) and keeps both
  implementations identical. Both datasets have such a patch.
- **Require independent colour evidence for every Verified mode.** It would downgrade 14 Verified
  modes whose single test file shows no chart, for a lack of fixtures rather than a defect, and
  independent evidence would still be unavailable for DNG profiles. The support table shows each mode's
  colour evidence instead.
- **An Adobe DNG SDK rendering as the colour reference.** It would give DNG profile colour a
  reference, but the SDK is not part of the build (ADR 0014), and it would still be a renderer's
  calibration rather than a measurement; the chart is independent of every renderer.
- **Fit a matrix to each chart and compare with it.** That measures the fit, not Lucent; the
  published values are the reference.
- **A fixed gate from the literature alone (for example mean 3).** The literature's numbers are for
  fitted matrices or rendered JPEGs under controlled light; the gate is derived from measurements of
  the corpus's own captures, as section 10 asks, with the literature as context.
