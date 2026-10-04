# ADR 0014: DNG opcode processing at the stages the specification defines

- **Status:** Accepted
- **Date:** 2026-10-04
- **Workflow:** W9 (DNG opcodes)
- **Amends:** ADR 0005 decision 8 (opcodes are no longer all `supported_by_lucent = false`) and
  ADR 0006 sections 1 and 5 (three opcode stages join the render path)

## Context

Blueprint section 3: "A missing required color interpretation, unsupported CFA, or mandatory DNG
opcode cannot silently become a verified image." Section 5, step 4: "Required DNG operations
follow their specified stage/order instead of this generic route. Record the geometry mapping,
normalize orientation consistently, and never apply the same DNG/vendor correction again through
Lensfun."

Until W9, Lucent's TIFF/DNG inspector parsed opcode-list headers only (ADR 0005 decision 8):
every DNG opcode was a `RequiredOperation` with `supported_by_lucent = false`, so a mandatory one
made the file `RequiresUnsupportedOperation`, and the render path did not apply any (ADR 0006
section 5). LibRaw 0.22.2 is built without the Adobe DNG SDK and applies no opcode list (it
maps a lossy DNG's OpcodeList2 MapPolynomial while unpacking, which ADR 0005 records as
`applied_by_decoder`). Four corpus DNGs were therefore "Unsupported", and GainMap-bearing DNGs
were rendered without their lens-shading correction.

The opcodes of the corpus (read 2026-10-04 with an independent parser,
`tests/tools/dng_opcode_reference.py`):

| Fixture | OpcodeList1 | OpcodeList3 |
|---|---|---|
| dji-fc4382-dng (Mini 4 Pro) | | WarpRectilinear (3 equal sets: kr0 0.99992, f(1) = 0.9946) |
| dji-fc7303-dng | | FixVignetteRadial (g(1) = 2.633), WarpRectilinear (f(1) = 0.965), GainMap 49 x 65 x 3 (0.969..1.159) |
| dji-fc220-dng | FixBadPixelsList (2169 points, BayerPhase 0) | FixVignetteRadial (g(1) = 3.197) |
| hasselblad-l2d-20c-dng | | GainMap 32 x 32 x 3 (1.000..2.658), WarpRectilinear (3 different sets: lateral CA) |

All are mandatory (flag bit 0 clear). None of the corpus files has an OpcodeList2, a
WarpRectilinear2, a TrimBounds, a MapTable/MapPolynomial or a per-row/per-column opcode; phone
DNGs (not in the corpus) commonly carry per-Bayer-phase GainMaps in OpcodeList2.

The specification is DNG 1.7.1.0 (September 2023), chapter 7 "Opcode List Processing" (pages
105-122): the list header and flags (p. 105), the per-opcode clip to the logical range "after the
application of each opcode" (0..2^16-1 for OpcodeList1, 0.0..1.0 for OpcodeList2 and 3, p. 105),
the WarpRectilinear2 skip rule (p. 105 and 121-122), and one section per opcode: WarpRectilinear
(106-108), WarpFisheye (108-110), FixVignetteRadial (110-111), FixBadPixelsConstant and
FixBadPixelsList (111-112), TrimBounds, MapTable (113), MapPolynomial (114), GainMap (115-116),
DeltaPerRow/Column and ScalePerRow/Column (116-119), WarpRectilinear2 (120-122). Chapter 7 does
not define the coordinate frames of the lists; the tags (chapter 4, OpcodeList1/2/3) place List
1 on "raw values exactly as read from the file", List 2 after mapping to linear reference values
and List 3 after demosaicing. The Adobe DNG SDK is not available in this environment, so
conventions the text leaves open are decided below and labelled as Lucent's.

## Decision

1. **Parameters are decoded by the raw module, strictly.** `raw::parse_dng_opcode_parameters`
   (`raw/dng_opcodes.hpp`) decodes the big-endian parameter areas of WarpRectilinear,
   WarpRectilinear2, FixVignetteRadial, FixBadPixelsConstant, FixBadPixelsList, TrimBounds,
   MapTable, MapPolynomial, GainMap and Delta/ScalePerRow/Column into typed values
   (`DngOpcodeParameters`). Every layout must fill its byte count exactly; counts are checked
   against the remaining bytes before anything is allocated (so no count read from a file can
   allocate more than the payload holds); every float and double must be finite; coefficient
   sets N in 1..4; BayerPhase 0..3; TableSize 1..65536; Degree 0..8; GainMap points and planes
   >= 1 with positive spacing where there is more than one point; per-row/column Count equal to
   the area's affected rows/columns; areas not inverted with planes and pitches >= 1; normalised
   optical centres in [0, 1]; WarpRectilinear2 `0 <= min_valid_radius < max_valid_radius <= 1`.
   The TIFF inspector decodes the parameters of every opcode whose ID it knows and whose version
   does not exceed 1.7.1.0 ("a DNG reader should never attempt to process an opcode with a version
   higher than DNG specification it was written to support", p. 105). A malformed parameter area
   is a `Corrupt` error of the parser (`raw.dng.opcode_parameters`); the inspector records it on
   the opcode (`DngOpcode::parameter_error`, which keeps `std::monostate` parameters) instead of
   failing the whole inspection, so the opcode is one Lucent cannot apply: a mandatory one makes
   the file `RequiresUnsupportedOperation` (never verified, as before W9) while the DNG's other,
   valid metadata (colour, levels, noise) is still used; an optional one is skipped with that note.
   The decoded parameters travel in `DngMetadata::raw_ifd.opcodes`; the inspector charges them
   against its value budget (an opcode list costs twice its byte size).

   WarpRectilinear2's parameter table (p. 120) is read as: N; per set 15 radial coefficients,
   kt0, kt1, min_valid_radius, max_valid_radius; then c^x, c^y and reciprocalRadial (one LONG),
   i.e. 4 + 152 N + 20 bytes. The extracted table leaves the indentation ambiguous and no corpus
   file carries this opcode; a file with another layout fails the exact byte count and is
   reported, never misread.

2. **The raw module decides what Lucent applies; the engine applies exactly that.**
   `raw::assess_dng_opcode` is the support table, evaluated on the image each list processes
   (`raw::dng_opcode_frame`, below); `raw::assess_dng_opcode_operations` sets
   `RequiredOperation::supported_by_lucent`, `superseded_by_warp_rectilinear2` and `lucent_note`
   and appends an interpretation issue only for an opcode that is neither applied nor legitimately
   skipped. The decoded-RAW contract becomes **1.3** (fields added; the decoder's pixel values are
   unchanged, the engine applies the opcodes). `check_consistency` rejects an operation marked
   supported that the table would not accept.

   | Opcode | List 1 (stored values) | List 2 (linear, pre-demosaic) | List 3 (demosaiced) |
   |---|---|---|---|
   | WarpRectilinear, WarpRectilinear2 | no | no | yes (N = 1 or one set per plane) |
   | FixVignetteRadial | yes | yes | yes |
   | FixBadPixelsConstant | yes (2 x 2 Bayer) | no | no |
   | FixBadPixelsList | yes (2 x 2 Bayer) | yes (2 x 2 Bayer) | no |
   | TrimBounds | no | yes (inside the image) | yes (inside the image) |
   | MapTable, MapPolynomial, GainMap, Delta/ScalePerRow/Column | yes | yes | yes |
   | WarpFisheye, unknown IDs, versions > 1.7.1.0 | no | no | no |

   Further conditions: List 1 needs 16-bit stored samples, the decoded frame equal to the raw IFD,
   and stored values the decoder has not changed (no LinearizationTable, not LibRaw's lossy-DNG
   tone curve); List 2 is unavailable when the decoder applied an opcode of List 2 (lossy DNG);
   Lists 2 and 3 need the visible area inside the ActiveArea; List 3 needs the colour planes R,
   G, B in that order (CFAPlaneColor absent or 0, 1, 2). Bad-pixel opcodes need the image's green
   sites where BayerPhase puts them (red/blue may swap: the repair uses the same lattice).

3. **Stages.** Both render paths (`engine::render_*`, ADR 0006, and the develop pipeline's stage
   one, ADR 0010) run `engine::DngOpcodeProcessor` (`engine/dng_opcode_processing.hpp`):

   | List | DNG model | Lucent stage | Stage record |
   |---|---|---|---|
   | 1 | raw values as read, before linearisation | the decoded raw frame (stored 16-bit samples), before normalisation | `dng_opcode_list1` |
   | 2 | linear reference values (after linearisation, black and white mapping), before demosaicing | the normalised visible area (`SensorImage`), before white balance | `dng_opcode_list2` |
   | 3 | after demosaicing | the demosaiced camera RGB (or monochrome) image, before camera colour (render) / orientation (develop) | `dng_opcode_list3` |

   An opcode stage exists when the file has that list and records every opcode of it (applied,
   skipped optional, not applied, superseded, applied by the decoder, disabled) with its
   parameters or reason, the samples written, the samples limited to the logical range, warp
   input positions outside the image and unrepaired bad pixels (`DngOpcodeRecord`, in
   `RenderReport::dng_opcodes`, `StageOneImage::dng_opcodes` and `DevelopReport::dng_opcodes`;
   the stage records' parameters carry one line per opcode, so `lucent-render --report` shows
   them). Opcodes run in list order. All kinds of stage one (interactive, export, proxies)
   apply the same opcodes at full resolution, so previews and exports agree. Preview-skippable
   opcodes are always applied. `kRenderPipelineVersion` and `kDevelopPipelineVersion` become 2,
   `kDngOpcodeProcessingVersion` is 1.

4. **Coordinates.** List 1 processes the whole stored raw image; Lists 2 and 3 the ActiveArea
   (the whole image without one), as in the DNG SDK's stage images. Lucent's buffer is the raw
   frame (List 1) or the visible area, which LibRaw may start one row or column inside an odd
   ActiveArea margin (ADR 0005); `dng_opcode_frame` gives the buffer's offset in the stage image,
   and every area, pitch, point, map coordinate, optical centre and radius is evaluated in stage
   coordinates (rows and columns the buffer does not hold are simply not written). A List 2
   TrimBounds makes the trimmed rectangle the stage image for the rest of List 2 and for List 3
   (the sensor image, its saturation mask and CFA are cut and `NormalizationRecord::source_area`
   follows).

5. **Values.** List 1 keeps the specification's 16-bit logical range: every result is rounded
   to nearest (halves away from zero) and limited to [0, 65535]; the real-valued opcodes see
   value / 65535 where the SDK converts to its float range (MapPolynomial x = v / 65535,
   Delta adds delta x 65535; ScalePer*/GainMap/FixVignetteRadial multiply). Lists 2 and 3 work on
   Lucent's normalised floats (1.0 = white). DNG clips to [0, 1] after every opcode; Lucent does
   not clip linear opcodes (gains, scales, deltas, warps), keeping its unclipped convention
   (results inside [0, 1] are identical; the export highlight clip and stage two's clip still limit
   highlights afterwards), but value maps are defined on the logical range only: MapTable and
   MapPolynomial limit their input and output to [0, 1] (MapTable on floats maps
   round(x x 65535), the SDK's 16-bit path). Limited samples are counted.
   * *White balance.* Lucent balances the mosaic before demosaicing, so List 3 receives camera
     RGB multiplied by the stage-one multipliers m_p. Gains, scales and warps commute with that;
     Delta* add delta x m_p and value maps act on v / m_p and multiply back, so every opcode acts
     on the unbalanced values the DNG model defines.
   * *Saturation.* WhiteLevel applies after List 1, so saturation is judged on the corrected
     stored values. List 2 opcodes correct measured values, and a saturated sample has none beyond
     "at least white": after List 2 every sample flagged saturated is raised back to its
     channel's saturation level when an opcode lowered it (`list2_resaturated_samples`), so
     highlights clipped in the sensor stay neutral after the highlight clip whatever gain an
     opcode applied (with the per-opcode [0, 1] clip of the SDK a gain below 1 would turn them
     grey or tinted).

6. **Per opcode.**
   * *GainMap:* bilinear in the grid at the pixel centre, v = ((row + 0.5) / H - MapOriginV) /
     MapSpacingV and h likewise with the width (the image being modified, not the area); indices
     are clamped to the grid ("outside the gain map bounds, values are replicated from the
     edges"); modified plane Plane + i uses map plane min(i, MapPlanes - 1) ("If Planes >
     MapPlanes, the last gain map plane is used for any remaining planes").
   * *FixVignetteRadial:* g = 1 + k0 r^2 + ... + k4 r^10 on every plane.
   * *WarpRectilinear / WarpRectilinear2:* the specification's formulas (p. 106-107, 120-121) with
     (x0, y0) = (0, 0) and (x1, y1) = (W - 1, H - 1), the stage image's corner pixel indices
     ("pixel coordinates of the top-left / bottom-right pixel"), so c = c^ (W - 1, H - 1) and m is
     the distance from c to the farthest corner pixel; plane i uses coefficient set
     min(i, N - 1); WarpRectilinear2 evaluates f(r) = sum k_p r^p at clamp(r, min, max) and divides
     by it when reciprocalRadial is set. The identity (kr0 = 1, all else 0) is not resampled. An
     undefined position (division by zero) is `Corrupt`. Output size is unchanged; positions
     outside the image take the edge (and are counted).
   * *Resampling kernel:* Keys cubic convolution with a = -0.5 (Catmull-Rom), separable 4 x 4
     taps at the exact fractional position, accumulated in double, taps beyond the buffer clamped
     to its edge pixels. When all coefficient sets are equal the position and weights are shared
     by the planes.
   * *TrimBounds:* the buffer is cut to the rectangle.
   * *FixBadPixelsConstant / FixBadPixelsList:* a bad pixel (value == Constant, or a listed
     point or rectangle) becomes the mean of its nearest same-colour good neighbours: green sites
     search rings of increasing distance on the green quincunx (sqrt 2, 2, sqrt 8, ...), red and
     blue sites on their 2-pixel lattice (2, sqrt 8, 4, ...), up to 16 pixels; the first ring with
     a good neighbour inside the buffer gives the mean (integers rounded half up). A bad pixel
     without one keeps its value and is counted. The DNG SDK's own interpolation is not
     reproduced (not available here); this is Lucent's documented rule.

7. **Geometry is recorded.** Every warp and trim appends a `GeometryChange` (stored orientation,
   before the orientation stage) whose `source_of(point, plane)` maps a continuous output point
   (image/geometry.hpp convention, pixel centres at i + 0.5) to the input point it shows, so masks
   and sensor diagnostics can be carried through (blueprint section 5, "Noise, masks, and
   geometry").

8. **Never twice.** An opcode the decoder applied is recorded (`AppliedByDecoder`) and never
   applied again; an opcode after an optional WarpRectilinear2 that Lucent applies is skipped as
   chapter 7 requires (when Lucent declines the WarpRectilinear2, for example for malformed
   parameters, the legacy warp is applied instead). For lens-profile corrections (Lensfun, not
   yet in Lucent), `engine::carried_lens_corrections(metadata)` states what the file's applied
   opcodes already corrected: distortion (any WarpRectilinear/2), lateral chromatic aberration
   (warp sets that differ per plane) and vignetting (FixVignetteRadial, or a List 2/3 GainMap
   covering the whole image). The future Lensfun stage must disable each carried correction and
   record that it did; it must run after List 3 (on the opcode-corrected geometry) and map its
   own geometry after the opcodes' `GeometryChange`s.

9. **Reference comparisons can switch opcodes off.** `RenderSettings` 1.1 adds
   `apply_dng_opcodes` (default true) for comparisons with decoders that apply none. When false,
   every opcode that would have been applied is recorded `Disabled` and becomes an issue of the
   report's interpretation (never reported complete); the develop pipeline has no such switch.

10. **Memory.** `estimate_render_memory` adds `opcode_peak`: a List 3 warp or trim needs a second
    RGB image, a List 2 trim a second sensor image and mask; the develop estimate includes it.

## Consequences

- The four drone DNGs are `Complete` (`LibRawFixtures.DroneDngOpcodesAreSupported`) and render
  with their corrections. `DngOpcodeFixtures.DroneDngsRenderWithTheirOpcodes` (engine fixture
  tests) renders each at full resolution with and without its opcodes and checks: every opcode
  applied, at its stage and in order; the vignette ratio equals the specification's g(r) to
  5.9e-8 (dji-fc220-dng, 4149 samples away from repaired bad pixels); 14 to 35 textured patches
  per warped file found in the unwarped render, by a normalised-cross-correlation block search,
  within 1 pixel of the formula's displacement (largest displacements 9.5 px fc4382, 52.7 px
  fc7303, 150 px Hasselblad); residual shift against an independent bilinear rendering of the
  formula below 0.1 px (measured 0.035, 0.050, 0.063 px); the geometry record equals the
  formula to 1e-9 px; determinism (parallel and serial bit-identical).
- **Comparison with rawpy / LibRaw.** LibRaw applies no opcodes, so `tests/tools/
  dng_opcode_reference.py` builds the reference: rawpy 0.27.1 (LibRaw 0.22.1) camera-space linear
  output (as-shot white balance, highlight clip, AHD), then the file's OpcodeList3 applied with
  NumPy/SciPy from the specification (read by its own TIFF parser; SciPy's cubic B-spline instead
  of Catmull-Rom). Measured 2026-10-04 on Lucent's stage one (export kind, as shot), 32 x 32 block
  means normalised by the median ratio (p99 of |ratio - 1| over unclipped, not dark blocks), and
  phase-correlation shifts of 64 x 64 patches:

  | Fixture | G p99 with/ref-with | G p99 plain/ref-plain (decoder baseline) | G p99 with/ref-plain (opcodes' effect) | shift with/ref-with median / max | shift plain/ref-plain | shift with/ref-plain |
  |---|---:|---:|---:|---|---|---|
  | dji-fc4382-dng | 0.40 % | 0.39 % | 10.3 % | 0.022 / 0.057 px | 0.012 / 0.043 px | 2.3 / 8.5 px |
  | dji-fc7303-dng | 0.15 % | 0.14 % | 87.8 % | 0.019 / 0.037 px | 0.014 / 0.025 px | 3.5 / 12.1 px |
  | dji-fc220-dng | 0.16 % | 0.15 % | 80.5 % | 0.010 / 0.027 px | 0.010 / 0.027 px | 0.010 / 0.027 px (no warp) |
  | hasselblad-l2d-20c-dng | 0.43 % | 0.41 % | 334 % | 0.024 / 0.138 px | 0.015 / 0.045 px | 16 / 31 px (window limit) |

  With opcodes, Lucent agrees with the opcode-corrected reference as closely as the two decoders
  agree without them (red and blue p99 0.5 to 1.9 %, same as their baselines); without the
  opcodes the vignetting and gain maps alone change blocks by 10 % to several hundred percent.
  The visual check (reduced JPEGs and full-resolution corner crops, before and after) shows the
  Hasselblad's dark circular corners and curved horizon corrected, dji-fc220's corner falloff
  removed and the corners of the warped DJI files magnified outwards.
- Cost on the reference laptop (Ryzen 5 5500U, another build running): WarpRectilinear 0.13 s
  (12 MP, shared sets) and 0.46 s (20 MP, per-plane sets), FixVignetteRadial 0.02 s, GainMap
  0.07 s, FixBadPixelsList (2169 points) 0.003 s. A warp holds a second RGB image at the List 3
  stage (`opcode_peak`), below the demosaic peak.
- The rawpy gates of `lucent_render_integration_tests` render with `apply_dng_opcodes = false`
  (the committed statistics in `render_reference.json` were measured without opcodes). Follow-up:
  `research/tools/render_reference.py` renders through `lucent-render --legacy-neutral-v0
  --dump-linear`, which now applies the opcodes; `lucent-render` needs a `--no-dng-opcodes`
  option (apps/lucent-render, outside this increment) before the statistics are regenerated.
- Items imported before W9 keep the interpretation status their import probe stored in the
  catalog ("unsupported" for the drone DNGs) until they are probed again; renders and exports
  already apply the opcodes.
- Every render of a file with opcodes changes (pipeline versions 2), so Lucent-rendered
  thumbnails are re-rendered. `kDngOpcodeProcessingVersion` is not yet appended to
  `kDevelopEngineVersions`: the application test that recomputes the thumbnail key lists that
  array's members explicitly; appending it is a follow-up together with that test. Until then a
  change of opcode semantics must also bump `kDevelopPipelineVersion`.
- Not supported (the file stays unsupported when such an opcode is mandatory; optional ones are
  skipped with a note): WarpFisheye; any opcode newer than DNG 1.7.1.0; List 1 on floating-point
  stored data or after the decoder linearised the values; TrimBounds in List 1; warps in Lists 1
  and 2; FixBadPixelsConstant outside List 1 and bad-pixel repair on non-Bayer CFAs; List 2 of a
  lossy DNG (LibRaw applies its MapPolynomial while unpacking); List 3 with CFA colour planes
  other than R, G, B. Also still open: DefaultCrop (DNG crops after the warps), ProfileGainTableMap,
  lens-profile corrections, and verification against the Adobe DNG SDK's own conventions (gain-map
  pixel centres, map-plane indexing, warp centre and edge handling, bad-pixel interpolation):
  where the text leaves them open, Lucent's choice is documented above.
- Revisit when a corpus file carries WarpRectilinear2, an OpcodeList2 or WarpFisheye, when the
  DNG SDK becomes available for a parity check, or when Lensfun corrections are added.

## Alternatives considered

- **Apply the opcodes in the decoder adapter.** Would change the decoded-RAW contract's promise
  that samples are exactly what the decoder produced (ADR 0005), would put List 2 and 3 (which
  need normalised and demosaiced data) in the wrong module, and would tie opcode support to
  LibRaw. Rejected: the raw module decodes and assesses, the engine processes.
- **Let the engine decide support itself.** The metadata's interpretation (catalog support
  states, import, the UI) must say whether a file is fully supported before anything is rendered.
  A single table in `raw::assess_dng_opcode`, enforced by `check_consistency` and by the
  processor (which applies exactly the supported opcodes), keeps the two from drifting.
- **Clip to [0, 1] after every opcode, as DNG states.** Bit-faithful to the SDK for in-range data
  but throws away values above white that Lucent's highlight handling and exposure keep, and turns
  saturated highlights grey or tinted under gains below 1. Lucent clips value maps only and keeps
  saturated samples at their level (decision 5).
- **Apply List 3 before white balance (unbalanced, as the DNG model has it).** Would need
  demosaicing of unbalanced data, which Lucent's Malvar-He-Cutler and X-Trans routes do not
  assume (ADR 0006); instead the white-balance multipliers are compensated where an opcode does
  not commute with them.
- **Resampling kernels.** The DNG text recommends "a suitable resampling kernel, such as a cubic
  spline". A cubic B-spline needs a global prefilter to interpolate (otherwise it blurs) and so a
  second full-image pass; Keys a = -0.75 (a common sharper choice) is only first-order accurate
  and overshoots more; Lanczos-3 needs 36 taps. Catmull-Rom interpolates, is
  third-order accurate (it reproduces quadratics exactly, which the unit tests use), is cheap and
  standard.
- **Fail the whole inspection on malformed opcode parameters** (the inspector's policy for
  malformed interpreted tags, ADR 0005). Tried first: the file then became `Ambiguous` and lost its
  valid DNG colour and level metadata, so it could no longer be rendered even for diagnostics
  (`allow_unsupported` exports), and the application's support reasons changed from "mandatory
  operation unsupported" to "DNG inspection failed". The opcode list itself is well formed; only
  one opcode's payload is not, and recording that opcode as unappliable keeps the file unverified
  with the precise reason.
