# ADR 0006: First render path: stage order, colour contracts and provisional display rendering

- **Status:** Accepted
- **Date:** 2026-09-26
- **Workflow:** W2 (First render path)

## Context

Blueprint section 5 asks for "one explicit image-processing pipeline": a recorded, ordered graph
in which every edge declares its colour encoding, a documented camera-to-reference transform,
float32 scene-linear Rec.2020 / D65 as the working space, and conversion to the output profile
only at encoding. Libraries must not "silently choose a different order". Section 8 asks for
versioned recipes that record operation identifiers, versions and parameters.

W2 has to turn a decoded RAW (ADR 0005) into a correct exported image before any editor exists
(W6). The pieces were built separately in W2: sensor normalisation, white balance and
demosaicing routes (`engine/sensor.hpp`, `engine/demosaic.hpp`), and the exporter
(`lucent::exporter`, which accepts display-referred RGB only and clips whatever lies outside
the output profile).
What remained was camera colour, a display rendering, orientation and the orchestration, plus
evidence that the result is colorimetrically right on real cameras.

Facts that shaped the decision (measured 2026-09-26 on the 34-file corpus):

- LibRaw reports a usable XYZ(D65)-to-camera matrix (`cam_xyz`) for every non-DNG fixture, but
  for DJI, Hasselblad L2D and Insta360 DNGs it is all zero: only the DNG tags describe the
  camera. Adobe-converted and Canon DNGs carry ForwardMatrix, CameraCalibration and
  AnalogBalance; LibRaw's processing does not use ForwardMatrix and renders with the D65
  matrix alone.
- The two monochrome DNGs have no colour tags at all.
- LibRaw's per-channel `linear_max` lies below the white level for many cameras (Canon R5 and
  5D Mark IV 14008 of 16383, Sony A7 IV 15360, Nikon Z 6 15311, a value LibRaw invents as
  maximum / 1.07 when the file has none), yet real data reaches the white level (R5: 31 samples
  at 16383, Z 6: 724, A7 IV uncompressed: 82,587). It is not a saturation level, and LibRaw's own
  processing never reads it.
- Some DNGs carry a negative BaselineExposure (Leica M10: -0.78 EV), which darkens the default
  rendering; the Adobe DNG SDK applies negative exposure as a tone curve that keeps white at
  white (`dng_function_exposure_tone`), not as a plain gain.

## Decision

### 1. Stage order and contracts

`engine::render_file` / `render_decoded` (`src/engine/include/lucent/engine/render.hpp`) run
these stages, each a separate pass over the pixels:

| # | Stage | Operation (algorithm id) | Output encoding |
|---|---|---|---|
| 1 | decode | `RawDecoder` (`libraw-decoder`) | camera-native mosaic / linear / mono, stored units |
| 2 | normalize | black/white normalisation (`black-white-normalisation`) | same, white = 1.0, unclipped |
| 3 | white_balance | channel multipliers + highlight clip (`channel-multipliers`) | same, balanced |
| 4 | demosaic | Bayer MHC / X-Trans baseline / bypass | camera-native linear RGB (or mono) |
| 5 | camera_color | camera -> XYZ (`camera-to-xyz-<path>`) | CIE XYZ, D50 (DNG) or D65, linear, scene |
| 6 | working_space | XYZ -> Rec.2020 with Bradford (`xyz-to-rec2020-d65-bradford`) | scene-linear Rec.2020 / D65 |
| 7 | display_rendering | Neutral v0 (`neutral-v0`, provisional) | display-referred linear Rec.2020 / D65 |
| 8 | orientation | EXIF orientation baked in (`exif-orientation`) | same, upright |

The exporter's `encode_for_output` receives stage 8's output (`display_linear_rec2020()`) and
converts it to the selected ICC output profile. Values are limited in exactly three places:
stage 3 clips raw highlights to the lowest white-balanced saturation level (default highlight
policy, section 3), stage 7 limits max(R, G, B) to 1 (section 4), and the exporter clips, per
channel, whatever lies outside the output profile's range after its conversion (out-of-gamut
components, negative or above 1). Every stage records its input and output `ColorEncoding`; the
pipeline requires each input to equal the previous output and fails with `Internal`
("engine.render.edge") otherwise. Each stage also records its algorithm id, semantic version,
parameters, pixel sizes, wall time and the number of samples it limited (`StageRecord`).

The exporter verifies every file before publishing it: a JPEG is encoded into memory and the
written file must equal those bytes exactly (then decode cleanly); a TIFF must decode to the
intended samples, compared row by row (`lucent/export/export.hpp`).

White balance precedes demosaicing (the MHC and X-Trans kernels assume balanced channels); the
blueprint's step 5 is therefore split: multipliers in stage 3, camera interpretation in stage 5.

### 2. Camera colour: three calibration paths, never a silent identity

`engine/camera_color.hpp`:

- **DNG profile** (the file is a DNG whose raw IFD Lucent inspected and whose IFD 0 has
  ColorMatrix1): DNG 1.7.1.0 chapter 6, structured like the Adobe DNG SDK's `dng_color_spec`:
  AnalogBalance and CameraCalibration folded into the colour matrices; two calibrations
  interpolated linearly in inverse correlated colour temperature (Robertson's method with the
  Wyszecki & Stiles table; illuminant temperatures from the SDK's table); the white found from
  the camera neutral by the spec's iteration (from D50, |dx| + |dy| < 1e-7, at most 30 passes,
  oscillations averaged); ForwardMatrix path when present, otherwise inverse ColorMatrix with
  Bradford adaptation to D50. Output is XYZ(D50).
- **Decoder matrix** (other files): LibRaw's `cam_xyz` with dcraw's row normalisation (each row
  divided by its product with the D65 white), inverted. This reproduces LibRaw's `rgb_cam`
  (tested against `cam_xyz_coeff` to 2e-6). Output is XYZ(D65).
- **Monochrome**: neutral, XYZ = value x D65 white.

No calibration (no ColorMatrix1 and a zero decoder matrix), a singular matrix, a neutral that
maps outside positive XYZ, or a DNG profile with other than 3 colour planes fails with
`Unsupported` / `Corrupt` at "engine.color.calibration" or "engine.color.dng".

The camera neutral is derived from the multipliers stage 3 actually applied
(neutral_c ~ 1 / multiplier_c); if the decoder had already balanced the data, the DNG's
AsShotNeutral supplies that balance. The applied white therefore maps exactly to the
calibration white, whatever the white-balance source. The matrix handed out applies to
white-balanced camera RGB: `CameraToXYZ * diag(1 / multiplier)`.

### 3. Exposure zero point

Scene-linear Y = 1 is the level at which a neutral surface brings the first camera channel (the
one with the smallest multiplier) to the white level: the DNG SDK's convention ("PCS white can
just be reached when the first camera channel saturates") and dcraw/LibRaw's with highlight
clipping. Implemented as `neutral_scale = 1 / min(multiplier_R, G, B)` folded into the camera
matrix. With this, Lucent's scene-linear values and rawpy's linear output agree without any
correction factor (measured luminance ratios 0.980 to 1.005).

Saturation (`engine/sensor.hpp`, normalisation version 2): a raw sample is saturated at or above
the white level, for every channel; the decoder's `linear_max` is recorded in the metadata but
never used (see Context). With the default highlight policy every white-balanced sample is
limited to the white level times the smallest multiplier (dcraw/LibRaw "highlight 0"), so a
fully clipped neutral has scene-linear level `scene_white = clip level x neutral_scale = 1`,
exactly where rawpy's clipped highlights are.

### 4. Neutral v0 display rendering (provisional)

`engine/display_rendering.hpp` (version 2). Per pixel of scene-linear Rec.2020: gain
`2^(exposure_ev + DNG BaselineExposure)` (exposure 0 by default; BaselineExposure only when the
file has it), then a hue-preserving mapping f on m = max(R, G, B) whose white point
`W = gain x scene_white` (the display-linear level of a fully clipped neutral) always maps to 1,
so clipped highlights are display white at every exposure and nothing is brighter than them:

- W > 1 (positive exposure): shoulder, identity up to the knee k = 0.8, then
  `k + (1 - k)(1 - (1 - (m - k)/(W - k))^p)` with `p = (W - k)/(1 - k)`; C1 at both ends.
- W = 1 (the default for files without BaselineExposure): identity below 1; only values above 1
  (saturated colours the camera matrix pushed beyond the clip level) are limited.
- W < 1 (negative exposure, e.g. Leica M10's BaselineExposure of -0.78 EV): the DNG SDK's
  exposure tone curve with slope s = W on u = m / W: the plain gain up to two stops below the
  clip point (u <= 1/4), then `a u^2 + b u + c` with `a = 16/9 (1 - s)`, `b = s - a/2`,
  `c = 1 - a - b` (checked against `dng_function_exposure_tone` in the SDK's `dng_render.cpp`;
  with scene_white = 1, s = 2^exposure exactly as there). C1 at the break.

The pixel is scaled by f(m)/m, so channel ratios are kept; negative (out-of-gamut) components
stay negative for the exporter. Version 1 used `W = max(1, gain x scene_white)`: with a negative
BaselineExposure (or a clip level below 1) clipped highlights stayed grey (Leica M10: sRGB 201)
and the image never reached white. There is no tone curve, contrast or saturation handling:
Neutral v0 is a placeholder until the W6 operations, is flagged `provisional` in every report
and has its own version (`kNeutralV0Version`).

### 5. Orientation, geometry and what is not applied

The file's EXIF orientation is baked into the pixels last (out of place unless Normal); the
exporter writes top-left images only. The rendered area is the decoder's visible area. Not yet
applied, and reported in `RenderReport::warnings` or the metadata's `Interpretation` instead of
being assumed: DNG opcode lists and other required operations, DNG DefaultCrop, non-square
pixels, lens corrections, noise reduction, sharpening, resizing.

### 6. Versioned settings and report

`RenderSettings` (schema `kRenderSettingsVersion` 1.0) holds everything that decides the pixels
of a decoded image: white-balance request, custom multipliers, highlight policy, exposure,
BaselineExposure switch, rendering id and knee, orientation switch. `validate()` rejects a newer
schema with `VersionMismatch` (core::check_readable), unknown enum values and out-of-range
numbers. `RenderReport` records the settings, `kRenderPipelineVersion`, the ordered stage
records, the interpretation status and issues, warnings, the memory estimate and the full
records of normalisation, white balance, demosaicing, camera colour (path, matrix, DNG white and
weights), working-space matrix and rendering. It is the forward-compatible seed of the W6
render record; `lucent-render --report` serialises it as `lucent.render-report` v1.

### 7. Memory and cancellation

Before decoding (from the probe) and again before normalising, `estimate_render_memory`
computes the peak of the pipeline's own buffers per stage (raw frame x 2 while decoding; raw +
float sensor + mask; sensor + mask + RGB while demosaicing; RGB plus the optional scene-linear
copy and one oriented copy). Above `RenderOptions::memory_budget_bytes` (4 GiB by default) the
render fails with `LimitExceeded` before allocating. Buffers are released as soon as possible
(raw frame after normalisation, mosaic and saturation mask after demosaicing). The exporter is a
separate budget: it holds the quantised image (3 or 6 bytes per pixel), the JPEG stream (grown by
doubling) or libtiff's strips, and one or two rows while verifying; `lucent-render` quantises,
releases the float display image, then writes. Cancellation is checked between stages
(`RenderOptions::on_stage_completed` reports each finished stage), per row inside every pixel
stage, every 64 scanlines in the codecs, per 4 MiB while writing and comparing JPEG bytes, while
hashing, and once more before publication.

### 8. Reference comparison and gates

`research/tools/render_reference.py` renders each fixture with rawpy (as-shot white balance, no
auto-brightening, linear 16-bit sRGB, highlight clip, `adjust_maximum_thr = 0`, LibRaw's default
demosaic, no `user_flip`: LibRaw applies the file's own orientation) and compares it with
`lucent-render --dump-linear` (scene-linear, linear sRGB primaries, float TIFF) on an
8x-downscaled grid; a size or orientation mismatch fails. It also compares two copies of
`dji-fc4382-dng` whose IFD0 Orientation is patched to 6 and 8 (no corpus file has a non-Normal
orientation). `tests/integration/data/render_reference.json` (schema 2) stores rawpy's 8 x 8
region means, its orientation and Lucent's measured statistics per fixture;
`lucent_render_integration_tests` then checks, per fixture:

- outer gross-error gates: region CIEDE2000 mean <= 2.0, max <= 6.0, luminance ratio 0.93 to 1.07;
- regression gates around the committed measurement: mean <= 1.25 x measured + 0.02, max <=
  1.25 x measured + 0.05, luminance ratio within 0.003 of the measured one;
- gate sensitivity (smoke tier): +-0.1 EV, white balance R x1.06 / B x0.94, a 5 % green gain and a
  red/blue swap applied to Lucent's own image must each fail the gates;
- the display image equals Neutral v0 applied to the scene-linear image, bit for bit, and every
  fully clipped neutral is display white;
- (smoke tier) the published JPEG and TIFF, decoded by independent libjpeg / libtiff readers,
  match the quantised image (TIFF exactly, JPEG within quantisation).

A separate test renders the patched DNGs end to end and requires the original image rotated as
EXIF 6 / 8 define. The rawpy comparison covers the scene-referred half; the display half is
checked against its definition, not against rawpy (whose output has no display rendering).

Measured 2026-09-26 after the W2 review fixes (saturation at the white level, Neutral v0
version 2; Lucent: LibRaw 0.22.2; reference: rawpy 0.27.1 / LibRaw 0.22.1). "Cell" columns are
CIEDE2000 per 8 x 8-pixel cell (rawpy uses AHD / its X-Trans route, Lucent MHC / its X-Trans
baseline); "Region mean" is the gated statistic (8 x 8 grid of region means). The `@exif6` /
`@exif8` rows are the orientation-patched copies of `dji-fc4382-dng`:

| Fixture | Calibration path | Lucent route | Cell CIEDE2000 mean | p95 | max | Luminance ratio | Region mean |
|---|---|---|---:|---:|---:|---:|---:|
| canon-eos-r5-cr3-raw | decoder_matrix | bayer_malvar_he_cutler | 0.106 | 0.237 | 2.03 | 1.0006 | 0.010 |
| sony-ilce-7m4-arw-compressed | decoder_matrix | bayer_malvar_he_cutler | 0.288 | 0.840 | 9.13 | 0.9994 | 0.057 |
| dji-fc4382-dng | dng_color_matrix | bayer_malvar_he_cutler | 0.634 | 1.387 | 3.95 | 0.9997 | 0.548 |
| fujifilm-x-t4-raf-lossless | decoder_matrix | xtrans_baseline | 0.207 | 0.626 | 10.12 | 0.9996 | 0.051 |
| nikon-z-6-nef-lossless | decoder_matrix | bayer_malvar_he_cutler | 0.069 | 0.165 | 2.35 | 1.0002 | 0.005 |
| canon-eos-r5-cr3-craw | decoder_matrix | bayer_malvar_he_cutler | 0.108 | 0.241 | 2.48 | 1.0006 | 0.011 |
| canon-eos-r5-cr3-raw-crop | decoder_matrix | bayer_malvar_he_cutler | 0.104 | 0.235 | 1.74 | 1.0005 | 0.008 |
| canon-eos-r5-cr3-craw-crop-dualpixel | decoder_matrix | bayer_malvar_he_cutler | 0.102 | 0.226 | 1.39 | 1.0006 | 0.008 |
| canon-eos-5d-mark-iv-cr2-raw | decoder_matrix | bayer_malvar_he_cutler | 0.122 | 0.313 | 4.91 | 1.0002 | 0.009 |
| canon-eos-5d-mark-iv-cr2-sraw | decoder_matrix | linear_color_bypass | 0.010 | 0.034 | 0.06 | 1.0001 | 0.007 |
| sony-ilce-7m4-arw-uncompressed | decoder_matrix | bayer_malvar_he_cutler | 0.273 | 0.770 | 8.27 | 0.9999 | 0.051 |
| sony-ilce-7m4-arw-lossless-l | decoder_matrix | bayer_malvar_he_cutler | 0.223 | 0.597 | 9.41 | 0.9998 | 0.027 |
| sony-ilce-7m4-arw-lossless-m | decoder_matrix | linear_color_bypass | 0.004 | 0.007 | 0.38 | 1.0002 | 0.003 |
| sony-ilce-7m4-arw-lossless-s | decoder_matrix | linear_color_bypass | 0.004 | 0.007 | 0.31 | 1.0002 | 0.003 |
| sony-ilce-7m4-arw-compressed-apsc | decoder_matrix | bayer_malvar_he_cutler | 0.321 | 0.836 | 6.82 | 1.0005 | 0.040 |
| sony-ilce-7rm5-arw-compressed | decoder_matrix | bayer_malvar_he_cutler | 0.150 | 0.396 | 7.16 | 1.0004 | 0.022 |
| dji-fc220-dng | dng_color_matrix | bayer_malvar_he_cutler | 0.244 | 0.456 | 1.80 | 0.9976 | 0.127 |
| hasselblad-l2d-20c-dng | dng_color_matrix | bayer_malvar_he_cutler | 0.703 | 1.754 | 10.83 | 1.0006 | 0.522 |
| dji-fc7303-dng | dng_color_matrix | bayer_malvar_he_cutler | 0.460 | 0.818 | 3.80 | 1.0047 | 0.369 |
| insta360-one-x-dng | dng_color_matrix | bayer_malvar_he_cutler | 0.663 | 1.526 | 10.39 | 0.9986 | 0.482 |
| nikon-z-8-nef-he | — | — | refused by both decoders | | | | |
| nikon-z-8-nef-he-star | — | — | refused by both decoders | | | | |
| nikon-z-6-nef-compressed | decoder_matrix | bayer_malvar_he_cutler | 0.069 | 0.165 | 1.89 | 1.0002 | 0.006 |
| fujifilm-x-t4-raf-compressed | decoder_matrix | xtrans_baseline | 0.203 | 0.608 | 9.95 | 0.9996 | 0.052 |
| fujifilm-gfx-50s-ii-raf-compressed | decoder_matrix | bayer_malvar_he_cutler | 0.221 | 0.777 | 8.79 | 0.9991 | 0.035 |
| panasonic-dc-s5m2-rw2 | decoder_matrix | bayer_malvar_he_cutler | 0.300 | 0.907 | 5.84 | 1.0003 | 0.046 |
| om-system-om-1-orf | decoder_matrix | bayer_malvar_he_cutler | 0.327 | 0.827 | 6.69 | 1.0004 | 0.056 |
| pentax-k-3-mark-iii-pef | decoder_matrix | bayer_malvar_he_cutler | 0.073 | 0.164 | 2.05 | 1.0001 | 0.003 |
| pentax-k-3-mark-iii-monochrome-dng | monochrome_neutral | monochrome_bypass | 0.001 | 0.003 | 0.01 | 1.0000 | 0.001 |
| leica-m10-dng | dng_color_matrix | bayer_malvar_he_cutler | 0.655 | 1.285 | 5.13 | 0.9982 | 0.624 |
| leica-m-monochrom-typ-246-dng | monochrome_neutral | monochrome_bypass | 0.002 | 0.004 | 0.01 | 1.0001 | 0.002 |
| hasselblad-x1d-ii-50c-3fr | decoder_matrix | bayer_malvar_he_cutler | 0.123 | 0.328 | 5.95 | 1.0002 | 0.011 |
| adobe-dng-canon-eos-5d-mark-iii-linear-dng | dng_forward_matrix | linear_color_bypass | 1.290 | 1.705 | 2.67 | 0.9798 | 1.327 |
| canon-eos-7d-mark-ii-dng-jxl | dng_forward_matrix | bayer_malvar_he_cutler | 2.171 | 4.214 | 9.06 | 1.0030 | 1.069 |
| dji-fc4382-dng@exif6 | dng_color_matrix | bayer_malvar_he_cutler | 0.634 | 1.387 | 3.95 | 0.9997 | 0.548 |
| dji-fc4382-dng@exif8 | dng_color_matrix | bayer_malvar_he_cutler | 0.634 | 1.387 | 3.95 | 0.9997 | 0.548 |

Reading the table: decoder-matrix files agree with LibRaw to a cell mean of 0.004 to 0.33 (the
remaining difference is demosaicing at edges, which is also where the cell maxima of up to 10
come from; with saturation at the white level the clipped highlights now agree, e.g. Canon R5
cell max 11.2 -> 2.0, Sony A7 IV lossless M/S region mean 0.09 -> 0.003). DNG files differ by
design: Lucent interpolates the two calibrations at the as-shot white and uses ForwardMatrix
when present, LibRaw uses the D65 matrix alone. The largest differences are the two
ForwardMatrix files, one of them shot at about 3700 K. On the smoke fixtures the simulated
errors of the sensitivity check reach region means of 0.61 to 1.04 (+-0.1 EV, also luminance
ratios 0.93 / 1.07), 1.04 to 1.44 (white balance R x1.06 / B x0.94), 1.14 to 1.73 (green x1.05)
and 5.8 to 17.2 (red/blue swap), against regression limits of 0.027 to 0.71.

## Consequences

- Every render states exactly what was done and what was not, so a W4 support table or a W7 job
  report can be built from it; the stage records are the template for the W6 operation graph.
- Separate passes cost about 0.1 s each on 45 MP (measured: whole engine 0.9 s for the Canon R5
  including a 0.54 s decode). Fusing passes is a later optimisation that must keep the recorded
  graph.
- Provisional and open items, all reported rather than hidden:
  - Neutral v0 is a placeholder look (no tone curve); W6 replaces it with recipe operations.
  - Highlights: clipped areas are flat display white (raw highlights are clipped to the lowest
    white-balanced saturation, as dcraw does). Highlight reconstruction is not implemented.
  - The X-Trans route is baseline quality; MHC is a validated reproduction, not the final Bayer
    choice (blueprint "Initial demosaicing decision").
  - DNG: opcode lists, DefaultCrop, ProfileGainTableMap, DNG 1.6 third illuminant and
    IlluminantData (illuminant 255), CameraCalibrationSignature matching and profile hue/sat or
    look tables are not applied. The camera neutral is used directly as the camera white
    instead of the SDK's value recomputed from the converged white (they agree to the search
    tolerance).
  - Non-square pixels are not resampled (a warning is emitted).
  - The memory estimate covers Lucent's pixel buffers only; decoder scratch memory and the
    exporter are outside it (measured peak working set of `lucent-render` for the R5: 736 MiB
    with JPEG export, 780 MiB with 16-bit TIFF export, against an estimated pipeline peak of
    726 MiB at the demosaic stage; before the exporter verified row by row and the tool released
    the float image before writing, 907 and 1238 MiB).
- Changing any numerical behaviour requires bumping the corresponding version constant
  (`kSensorNormalizationVersion`, `kWhiteBalanceVersion`, `kCameraColorVersion`,
  `kWorkingSpaceVersion`, `kNeutralV0Version`, `kRenderPipelineVersion`) and regenerating the
  reference statistics (`render_reference.py --tier all --write-stats`), which the regression
  gates compare against.
- Export publication leaves a `~lucent-*.tmp` file behind only when the process is killed
  outright (closing the console, log-off and shutdown cancel and wait for cleanup); sweeping such
  files, and holding the reserved temporary file's handle for the whole protocol instead of
  reopening it by name, belong to W7's publication journal.

## Alternatives considered

- **LibRaw's own processing (`dcraw_process`) for the whole path.** Rejected: a fixed order and
  hidden choices (automatic white level adjustment, its own highlight handling, 16-bit
  integer intermediates, sRGB-only matrices), exactly what the blueprint forbids. LibRaw remains
  the decoder and the reference.
- **LibRaw's `rgb_cam` / `cmatrix` for DNG files too.** Rejected: LibRaw's processing does not
  use ForwardMatrix or dual-illuminant interpolation, and its `cam_xyz` is zero for some DNGs
  (DJI, Insta360); following the DNG specification is the documented camera-profile transform
  the blueprint asks for.
- **One fused camera -> Rec.2020 matrix pass.** Deferred: cheaper by about 0.04 s on 45 MP, but
  the XYZ edge would no longer exist as data; kept separate while the graph is being
  established.
- **Normalising exposure so that the clip level maps to 1.** Rejected: it would change
  mid-tones per file (by 1 / clip level) and depart from the DNG and dcraw zero point that makes
  the rawpy comparison exact. Likewise, scaling the whole image so that W becomes 1 when a
  negative BaselineExposure makes W < 1 would cancel the BaselineExposure; the DNG SDK's
  exposure tone curve keeps the mid-tone darkening and still reaches white.
- **Saturating at the decoder's `linear_max`** (the first W2 implementation). Rejected: LibRaw's
  value is sometimes invented and real data lies above it (Context); clipping there discarded up
  to 0.23 EV of highlights and made clipped areas grey.
- **A filmic or ACES-style tone curve as the default rendering.** Rejected for W2: it is a
  photographic choice that belongs to W6's recipe operations; a near-identity shoulder keeps the
  export close to the scene-referred data and to the reference.
