# ADR 0011: Natural v1 automatic settings: metric contract, proposals, bounded search and confidence

- **Status:** Accepted (amended 2026-10-04 for phase 2)
- **Date:** 2026-10-03
- **Workflow:** W8 (Natural v1 optimiser), phase 1 of 3 (core) and phase 2 of 3 (related-shot
  consistency, recheck speed-ups, evaluation tooling: see "Amendment: phase 2" below); the
  application's "Auto" and batch flows are wired once W7's worker lands

## Context

Blueprint section 6 ("Automatic settings for every photograph") asks Lucent to choose "the best
evaluated candidate that meets calibrated technical constraints and the selected photographic
preference", without claiming "a global mathematical optimum or a universally correct artistic
edit", and to "preserve blue hour, warm sunsets, fog, snow, deep shadows, and deliberate
silhouettes instead of forcing every image toward a middle-bright neutral look". The first policy is
**Natural v1**: "small plausible corrections, restrained color, protected highlight gradients, and
limited noise amplification. Source uncertainty reduces edit strength." The section fixes the
procedure (decode once; a 1536-pixel analysis image and a 512-pixel search proxy; as-shot baseline,
identity, histogram/headroom and white-balance proposals; a bounded deterministic search of about
24-32 proxy candidates respecting locks; ranking of feasible candidates; recheck of the leading
three at analysis resolution and on native crops), the initial search bounds (exposure -1.5..+1.5
EV, white-balance gains +-10 % in the route's neutral representation, shadows 0..+0.75 EV,
highlights -0.75..0 EV, contrast narrow around identity, saturation 0.85..1.15) and a metric
contract: "Do not ship an arbitrary weighted sum of unrelated unnormalized quantities. Define each
metric's domain, mask, units, baseline, failure threshold, and missing-data behavior", "A metric
with too little evidence cannot receive an artificially good score", "Report source clipping, total
output clipping, and newly induced clipping separately", "Prefer the smaller edit when scores are
effectively tied", and "Manual locks apply throughout candidate search ... fallback". Section 5
requires tracking "source saturation before resampling". Section 10 requires numerical thresholds to
be set "using calibration data before final qualification" and the Natural results gate to be a
"blind preference and correction-effort evaluation against fixed rendering, a shared preset,
LUT-only enhancement, and the proposed optimizer"; Appendix D4 says "Freeze metric normalization,
bounds, confidence thresholds, and style-policy versions only after calibration".

ADR 0010 delivered what the optimiser renders with: `DevelopSettings` v1 (exposure, white balance as
shot / Kelvin / camera-neutral multipliers, guided-filter highlights and shadows, contrast, tone
curve, saturation and vibrance, base look "neutral" v1, locks), an interactive stage one that keeps
highlights unclipped so stage two can apply any white balance, `make_proxy`, and a stage two that is
per pixel except for the local-tone guided filter (radius = long edge / 64, a footprint of 2r).

No calibration corpus exists yet: the fixture manifest records scene diversity (sunset, blue hour,
night, fog, snow, high ISO, backlight) as absent, and the cached fixtures are mostly one daylight test
shot per camera. Natural v1 therefore ships as a documented, **uncalibrated** heuristic; this ADR
records what it is and how it will be calibrated.

## Decision

1. **Module.** `lucent_analysis` (`src/analysis`, namespace `lucent::analysis`, public dependencies
   core, image, raw and engine; no third-party library) holds the metric contract
   (`metric_contract.hpp`), source diagnostics (`source_diagnostics.hpp`), render measurements
   (`render_metrics.hpp`), proposals (`proposals.hpp`), the Natural v1 objective and hard checks
   (`objective.hpp`), native crops (`native_crops.hpp`) and the controller (`auto_settings.hpp`:
   `auto_develop()`, `auto_develop_file()`, `estimate_auto_memory()`). Results carry the policy
   ("natural" version 1), search version 1, metric-contract version 1 and the develop pipeline and
   settings versions.

2. **One decode, three scales.** The decoded samples are measured first (per-channel saturation,
   saturation coverage of the analysis and proxy grids, the noise model), then consumed by one
   stage one (Interactive, as shot, full resolution). The analysis image (1536 px long edge) and the
   search proxy (512 px) are `make_proxy` downscales of it; four native crops are cut from it with
   the local-tone halo, and the full-resolution image is released. Candidates are stage two on those
   cached images; nothing is decoded or demosaiced twice.

3. **Metric contract** (the specification table is `metric_specs()`; thresholds are constants the
   code uses, and a test pins them to the table). Clipping, blocked shadows and gamut are judged in a
   declared output encoding, **sRGB 8-bit** (the default export and screen encoding).

   | Metric | Domain / encoding | Mask | Units | Baseline | Thresholds (Natural v1) | Min. samples | Too little evidence |
   |---|---|---|---|---|---|---|---|
   | Numerical validity (hard) | rendered display-linear Rec.2020 and the settings | all samples | non-finite count, curve monotonicity | absolute | any failure | 1 | failure; a non-finite baseline fails the analysis (`Internal`) |
   | Source saturation (report) | decoded samples, stored units, visible area, before resampling | every site, per channel index | fraction at or above the white level; per analysis pixel coverage | absolute | flag above 0.1 % of a channel | 1 | n/a |
   | Render clipping (hard + pref.) | sRGB 8-bit | every pixel; source-saturated pixels reported, never excluded | fraction at code 255: total, new, source | baseline render | new <= 0.5 % | 4096 px | only the baseline stays feasible; flag |
   | Shadow blocking (hard + pref.) | sRGB 8-bit | every pixel | fraction with every channel at code 0 | baseline | new <= 0.5 % | 4096 px | as above |
   | Tonal distribution (pref.) | display lightness L*/100 of the render's luminance; scene headroom from stage one | every pixel; evidence = scene signal > 3 sigma | quantiles p1..p99.5, fractions, EV | baseline and a key-aware band | - | 4096 px and 10 % of the image | brightness term becomes an edit penalty; confidence 0; flag |
   | White balance (hard + pref.) | residual camera multipliers vs as shot; CIELAB a*b* of evidence-neutral surfaces | surfaces whose camera chromaticity matches the estimated cast (else C* < 5) | log gains, Delta E*ab, confidence | as shot | neutral shift <= 8 Delta E | 512 px | a residual is infeasible; flag |
   | Shadow noise (hard + pref.) | predicted L* noise: sensor model through the render's local gain | baseline 0.02 < L*/100 < 0.35, unsaturated | L* (p75), ratio | baseline | fail when > 4 L* and > 1.5x | 1024 px | no model: penalty on lifts; flag. Few shadows: not applicable |
   | Native detail (hard + pref.) | stage two on native crops (with halo), central 256 px | strong step edges with a plateau side; detail and shadow crops | halo L*, texture ratios | baseline crops | halo <= 4 L*; shadow texture > 4 L* and > 1.5x fails | 64 edges | local-tone edits penalised; flag |
   | Colour / gamut (hard + pref.) | linear-light chroma; CIELAB hue; sRGB gamut | chromatic baseline pixels (C* >= 8, 0.25 < L*/100 < 0.95) | log ratio, degrees, fractions | baseline | new out of gamut <= 2 %; hue shift <= 6 deg | 1024 px | saturation edits penalised; monochrome n/a |

   The noise model (`source_diagnostics.hpp`): variance of a normalised green sample x is
   read + shot x, from the DNG NoiseProfile when present; otherwise the read noise measured on the
   optical-black margins (1.4826 MAD of green sites, median within 2 % of the black level) with the
   shot noise of an assumed full well of 20000 e- at ISO 100 scaled by ISO; otherwise both from the
   ISO assumption (read 3e-4 of white x sqrt(ISO/100)); otherwise none. Predicted L* noise at a pixel
   is 100 l'(Y) (Y / x) sigma(x), native-resolution noise carried by the render's secant gain.

   Two measurement choices came from failures found while testing: the saturation change is linear
   chroma |d - Y| / Y, which every tone operation leaves exactly unchanged (CIELAB C*/(L*+16) moved
   with exposure in the dark linear segment and dragged saturation along); and the halo measure
   compares the edit's lightness change 2-4 px from a strong edge with its change 16-32 px away on
   sides that are plateaus in the baseline (an overshoot-beyond-far-ends measure read every thin
   line or gap narrower than the profile as a halo). It is zero for the baseline, close to zero for
   any per-pixel tone change (a plateau side's near and far lightness differ by at most 3 L*), and
   sensor noise cancels because both renders share the pixels.

4. **Proposals** (`proposals.hpp`). *Exposure/headroom:* the median luminance lightness is moved into
   a key-aware band [0.40 - 0.38 low_key, 0.85], low_key = clamp((p99 - 0.75) / 0.15) x
   clamp((0.30 - p50) / 0.15) (bright content near white with a dark median: silhouettes, night,
   low key), exactly in display luminance, limited by the scene headroom (p99.5 of the value norm
   against the stage-two clip level) minus 0.3 EV; what headroom withholds becomes half as much
   shadow lift; a brightening of an image with highlights near white proposes -0.25/-0.5 EV of
   highlight compression. *White balance:* three estimators on the proxy (area averaging attenuates
   demosaicing artefacts): first-order grey edge with p = 6 on a 3x3 binomial smoothing, bright
   neutral (median log ratios of the brightest 20 % within a +-0.5 log cast window) and a grey world
   restricted to that window. The cast is their median; agreement exp(-(spread / (0.05 + 0.25
   |cast|))^2) times the mean estimator confidence is the proposal's confidence. Natural v1 corrects
   the warmth axis by at most 0.045 log (fully up to 0.03, a quarter of the next 0.06) and the tint
   axis by at most 0.08 log (fully up to 0.05, half of the next 0.06), scaled by the confidence and
   bounded to +-10 % gains: small casts are corrected, warm sunsets and blue hours keep at least
   85 % of their colour.

5. **Search** (`auto_settings.cpp`). Coordinates and mapping: exposure_ev; white balance as camera
   multipliers relative to as shot (`CameraNeutralWhiteBalance`, the route's neutral representation,
   never reused across cameras) searched as a strength 0..1 along the proposal; shadows_ev /
   `kShadowsMaxEv`; highlights_ev / `kHighlightsMaxEv`; contrast; saturation multiplier - 1.
   Monochrome data has no white-balance or saturation coordinate. Candidates: the baseline (as shot
   for unlocked searched controls, locked ones at their values, tone curve, vibrance and base look
   carried over), the identity (the current settings), the exposure, white-balance, global
   (exposure + white balance) and combined (+ local tone) proposals, then coordinate descent from the
   best evaluated point in the fixed order exposure, shadows, highlights, white balance, contrast,
   saturation, steps (0.375 EV, 0.25 EV, 0.25 EV, 0.5, 0.075, 0.075) halved after each pass, until 32
   proxy renders. Locked coordinates never move; already evaluated settings are not rendered again.
   Hard checks gate feasibility; the leading three feasible proxy candidates and the baseline are
   re-rendered at analysis resolution and on the native crops and the best feasible one wins.

6. **Objective** (`objective.hpp`). A sum of dimensionless terms, each a measured quantity divided by
   the amount Natural v1 treats as one clearly visible defect, mostly squared, unit weights except the
   edit term (0.15): brightness (distance of the median from the band / 0.06), highlights (new
   near-white crowding / 0.05, new clipping / 0.0025, greying of sensor-clipped areas / 0.05), shadows
   (new blocking / 0.0025, near-black fraction / 0.30 weighted by (1 - low_key) / 2), noise (predicted
   noise above max(baseline, 2 L*) / 1 L*), contrast (|log IQR ratio| beyond 0.10 / 0.15), colour
   (|log saturation factor| beyond 0.03 / 0.08, new out-of-gamut / 0.005), white balance (log gain
   distance to the proposal / 0.02), native (halo / 1.5 L*, shadow texture above max(baseline, 2 L*),
   detail loss below 0.85 / 0.15) and edit size (normalised L1 over the search ranges). Only
   increases over the baseline count where the scene itself is the cause (crowded highlights of
   snow, colour outside sRGB): absolute terms pushed high-key and colourful scenes darker and less
   saturated in testing. Totals within 0.02 are ties, won by the smaller edit, then the earlier
   candidate. Missing evidence replaces a term by a penalty proportional to the edit it governs, so
   a candidate never scores better for lack of evidence.

7. **Confidence, restraint and flags.** Confidence is the smallest of the evidence confidences that
   apply to what the winner changes: source interpretation (unbalanced data 0.6, incomplete
   interpretation 0.7, more than 5 % of a channel saturated 0.85), brightness evidence, white-balance
   evidence (for a residual above 0.5 %), the noise model (DNG profile 1, measured read noise 0.9,
   ISO assumption 0.7, read noise only 0.6, none 0.3) for brightening, native crops (0.8 when none).
   Below 0.5, each group of controls moves towards the baseline by max(0.25, confidence / 0.5) by the
   evidence governing it (tone and colour controls; white balance), the restrained candidate is
   re-verified, and the result is flagged `LowConfidence` with the reasons. Review flags:
   LowConfidence, InsufficientEvidence (per metric), SourceSaturation, LockedControlsClip,
   WhiteBalanceUncertain, NoiseModelUnavailable, VisibleShadowNoise, NativeChecksUnavailable,
   NoFeasibleCandidate, InterpretationLimited, RecheckRejected (persisted numeric codes, append only).

8. **Result and determinism.** `AutoResult` holds the chosen `DevelopSettings` (current locks kept),
   the versions, confidence and its breakdown, flags with reasons, per-metric diagnostics of the
   chosen settings at analysis resolution (source saturation first), the source diagnostics, both
   proposals, the crops, every evaluated candidate with its proxy and analysis scores and failures,
   timings and memory figures. The same decoded input, settings and options give the same result:
   renders and reductions are schedule independent (fixed row bands merged in order, integer
   histograms), candidates are generated and ranked in a fixed order, there is no randomness; a test
   compares two runs and serial with parallel execution exactly.

9. **Errors.** Invalid options or current settings are `InvalidArgument` (`analysis.auto`); the
   decoder's, stage one's and proxies' errors pass through; a numerically invalid baseline is
   `Internal`; a candidate that fails to render is recorded as infeasible; `Cancelled` and
   `OutOfMemory` stop the analysis.

### Measurements

Reference laptop (Ryzen 5 5500U, 12 GB), `dev-core` (RelWithDebInfo), smoke-tier fixtures,
`lucent_analysis_fixture_tests`, 2026-10-03, while three other agents were building on the same
machine (upper ends pessimistic; second runs of the same file varied by up to 50 %). Each file was
analysed twice with identical results.

| Fixture (smoke tier) | Size | Analysis time, first / second run | of which decode / search / recheck | Candidates (proxy / analysis / crop renders) | Peak working set (estimate) | Chosen edit | Confidence, flags |
|---|---|---|---|---|---|---|---|
| Canon EOS R5 CR3 | 8191 x 5463 | 3.65 / 3.42 s | 0.55 / 0.89 / 1.27 s | 32 / 4 / 16 | 743 MiB (725) | +1.50 EV, shadows +0.66 EV | 0.90; WB uncertain |
| Sony ILCE-7M4 ARW | 7028 x 4688 | 2.61 / 1.74 s | 0.24 / 0.63 / 0.58 s | 32 / 3 / 12 | 552 MiB (534) | +0.05 EV (no headroom) | 1.00; source saturation 0.26 %, WB uncertain |
| DJI FC4382 DNG | 4032 x 3024 | 1.96 / 2.27 s | 0.32 / 0.46 / 0.56 s | 32 / 4 / 16 | 232 MiB (216) | +0.25 EV, highlights -0.13 EV | 0.70; WB uncertain, interpretation limited (opcode) |
| Fujifilm X-T4 RAF | 6246 x 4170 | 3.75 / 3.42 s | 1.91 / 0.42 / 0.66 s | 32 / 4 / 16 | 440 MiB (422) | shadows +0.38 EV (headroom 0.67 EV) | 0.70; WB uncertain, interpretation limited (X-Trans baseline) |
| Nikon Z 6 NEF | 6064 x 4040 | 1.78 / 2.08 s | 0.53 / 0.31 / 0.29 s | 32 / 3 / 12 | 414 MiB (397) | as shot (median already in band) | 1.00; WB uncertain |

The peak working set is that of a test process analysing one file (decode included); the estimate is
`estimate_auto_memory()` from the probe, which the controller's own figure matches. After the crops
are cut, the controller holds about 76-85 MiB (analysis image, proxy, crops, contexts and baseline
features); the full-resolution stage one dominates the peak. A rendering of the R5 file with the
chosen settings was inspected against the as-shot rendering: the dim indoor still life (median
L* 19.6) is brightened naturally, the lemon's highlights stay unclipped and the dark table still
reads as dark.

Synthetic scenes (`lucent_analysis_tests`, 960 x 640, about 0.5-1 s each): a chart 2.5 EV under is
brightened by the exposure bound (+1.5 EV) and 0.375 EV of shadow lift; a silhouette and high-key
snow keep their key (snow is pulled from L* 92 to the band's top, 85.7, by -0.29 EV of exposure); a
warm sunset (camera cast x1.35 red, x0.70 blue) keeps 90 % of its cast while a 5 % magenta tint is
corrected with red and blue gains of 0.966 and 0.960; a sensor-clipped sky is left as shot; locks
hold for every candidate;
a lens-cap frame keeps the baseline with insufficient-evidence flags; without a noise model the
brightening is restrained from +1.5 to +0.9 EV and flagged.

## Consequences

- Natural v1 is reproducible and inspectable: every candidate, term and failed check is in the
  result, so a reviewer can see why a photograph was brightened or left alone.
- On the five smoke fixtures the three white-balance estimators disagree (spreads 0.24-0.40 log; the
  grey edge leans blue on scenes with large coloured areas), so white balance stays as shot with a
  `WhiteBalanceUncertain` flag: the camera's white balance remains the default, as the blueprint
  wants until a calibrated estimator exists. Synthetic scenes exercise the correction path.
- Without semantics a backlit subject in front of a bright sky reads as low key and is not
  brightened beyond the bounded shadow lift; this is the documented price of preserving
  silhouettes.
- The recheck dominates the analysis time on 45-megapixel files (four native crops of about 840 px
  with the 2r local-tone halo per candidate). Rendering crops without the halo for candidates without
  local tone, and merging the three source passes into one, are the obvious speed-ups if W7's batch
  throughput needs them. (Phase 2 implemented the halo-free crops and two other recheck speed-ups;
  the source passes are unchanged.)
- Needs recorded for other modules (phase 2): an application `RevisionOrigin` for automatic settings
  and a note carrying the policy and search versions with each auto revision; the job journal (W7)
  should persist `AutoResult`'s flags, confidence and versions; the UI's "Auto" runs
  `auto_develop_file` in the worker with the item's current settings (locks included).
- Related-shot consistency (blueprint section 6) and learned proposals are not part of this phase
  (consistency: phase 2, below; learned proposals remain out of scope).

### What is uncalibrated, and how it will be calibrated

Everything numeric in Natural v1 is an engineering default: the hard thresholds
(`metric_contract.hpp`), the term scales, weights and tie tolerance (`objective.hpp`), the key and
band rules and the white-balance correction caps (`proposals.hpp`), the noise-model assumptions
(`source_diagnostics.hpp`), the confidence values and the restraint threshold (`auto_settings.hpp`).
None is a measured limit and none may be cited as one. Calibration follows blueprint section 10 and
Appendix D4:

1. *Corpus.* Rights-cleared RAW files covering the scene list of section 10 (daylight, sunset, blue
   hour, foliage, water, mountains, fog, snow, night, high ISO, backlight, saturated subjects,
   deliberate low/high key) from the qualification camera matrix, split by camera model and
   trip/scene with near-duplicates and brackets kept together, and held-out cameras.
2. *Ground truth for sub-models.* Grey-card or colour-checker frames per camera and illuminant for
   the white-balance estimators (angular error against the measured neutral); flat-field and dark
   frames per camera and ISO for the noise model (photon transfer), compared with DNG NoiseProfile
   where available.
3. *Preference.* Blind pairwise preference and correction-effort sessions with photographers on the
   training split: the fixed as-shot rendering, a shared preset, (later) LUT-only enhancement and
   Natural v1; correction effort is the slider distance from each output to the photographer's own
   finished settings. Term scales and weights are fitted to the pairwise preferences (for example a
   Bradley-Terry model over the objective's terms) subject to the defect budget; hard thresholds are
   set from annotated defects (visible new clipping, halos, noise).
4. *Freeze and test.* Freeze the scales, thresholds, confidence rules and search bounds as policy
   version 2, record them, and report preference, correction effort, defects and confidence by camera
   and scene on the held-out split before any claim of quality.

## Alternatives considered

- **A weighted sum of raw metrics.** Ruled out by the blueprint; every term here is normalised by a
  declared scale and hard checks gate feasibility first.
- **A global optimiser (Nelder-Mead, Bayesian optimisation, random restarts).** Larger budgets or
  randomness for little gain in six bounded dimensions; deterministic coordinate descent with
  proposal seeds is reproducible, cheap and explainable.
- **Searching at analysis resolution.** About nine times the pixels of the proxy per candidate; the
  proxy search with an analysis-resolution recheck of the leaders keeps the budget.
- **An exhaustive grid.** Even three values per coordinate is 729 renders.
- **Learned proposals (Exposure, HDRNet, adaptive 3D LUTs) or FFCC for white balance.** Research
  references in the blueprint, neither reproduced nor domain-validated for Lucent's renderer; the
  controller accepts more proposals later.
- **Kelvin/tint for the white-balance residual.** Unavailable on some calibrations; camera-neutral
  multipliers exist for every colour route and are already excluded from cross-model paste.
- **CIELAB chroma for the saturation change, an overshoot halo measure, absolute gamut and
  highlight-crowding terms, one shared restraint factor.** Each was implemented and replaced after
  the synthetic scenes exposed it (exposure-dependent chroma, thin structures read as halos, darker
  and duller results for colourful and high-key scenes, a weak white-balance estimate restraining a
  well-supported exposure change).

## Amendment: phase 2 (2026-10-04) - related-shot consistency, recheck speed-ups, evaluation tooling

### Context

Blueprint section 6, "Related-shot consistency": "Group conservatively using capture time and visual
similarity, with user override. Treat consistency as a soft penalty on appearance, never a shared
exposure setting for the whole folder. Large illumination changes split a group. Within a known
camera/calibration domain, residual WB changes may be compared; across cameras compare
color-managed neutral/chromaticity and rendered appearance instead of averaging camera gains.
Locked/manual images can serve as anchors only with explicit user selection." Step 5 of the search:
"If enabled, regularize related-shot upstream settings and revalidate them". Section 10's Natural
gate is a "blind preference and correction-effort evaluation", which needs a way to show reviewers
as-shot and automatic renders without telling them which is which. Phase 1 left three known limits:
the white-balance estimators often disagree (0.24-0.40 log spread), so white-balance edits are mostly
restrained; the recheck dominates the time on 45 MP files; backlit subjects read as low key.

### Decision

10. **Session** (`auto_settings.hpp`). `AutoSession::open()`/`open_file()` is `auto_develop()` that
    keeps what the analysis built: the analysis image, the search proxy, the native crops (with and
    without the halo), contexts, baseline features and every evaluated candidate. That is 50-90 MiB
    for 24-45 MP files. `independent()` is exactly `auto_develop()`'s result (`auto_develop()` now
    runs a session and returns it). `regularise(target)`, `appearance_of(settings)` and
    `preview(settings, long_edge)` reuse the kept images, so nothing is decoded or demosaiced again.

11. **What consistency compares** (`consistency.hpp`). Each candidate score carries an
    *appearance*, measured with the render's other statistics:
    - the median display lightness;
    - the rendered colour balance: CIE 1976 u'v' of the display XYZ summed over the baseline's
      mid-tones (0.15 < l_base < 0.9, unclipped). This is colour-managed and, being a ratio of
      sums, independent of exposure;
    - the log camera multipliers of its white balance, which are comparable only within one
      calibration domain.

    Each image carries *shot features*:
    - its capture time;
    - its camera and calibration domain: the camera, the colour route
      (`engine::CalibrationPath`) and a fingerprint of the calibration matrices the route uses;
    - its scene luminance: EV100 from f-number, exposure time and ISO, plus
      log2(median balanced-green signal / 0.127);
    - its as-shot white, converted to CIE 1960 uv through the file's own calibration;
    - its as-shot render's u'v';
    - three similarity descriptors: an 8 x 8 grid of exposure-normalised log scene signal, an
      8 x 8 grid of u'v' minus the image's own (content colour layout), and a 16-bin histogram of
      log2(signal / median).

12. **Grouping** (`related_shots.hpp`, `group_related_shots`). Caller overrides come first: items
    with a user group id form exactly that group, whatever their time, look or light, and never join
    automatic groups; an id unique to one item keeps it alone. The other items are taken in
    capture-time order. An item joins the open group only when all of these hold:
    - its gap to the group's last shot is at most 60 s, and the group's span stays within 15 min;
    - it is similar to that last shot: same orientation, aspect within 10 %, layout within
      0.75 EV per cell, colour layout within 0.010 u'v', tone histograms within a total variation of
      0.30;
    - no illumination change separates it from **any** member (complete linkage, so light cannot
      creep through a chain): scene luminance within 1 EV, as-shot whites within 0.008 uv, as-shot
      renders within 0.010 u'v'.

    Without f-number, exposure time or ISO a change of light cannot be ruled out, and the item
    starts a new group. Items without a capture time stay alone. Cameras may mix, since only
    colour-managed and exposure-normalised features are compared. `GroupingMode::None`
    (`--group-by none`) turns automatic grouping off; caller groups are explicit and still apply
    (the CLI has none, so there every item stays alone). Every decision is recorded with its
    measurements and reason.

13. **Targets** (`consistency_targets`), for groups of at least two analysed members.
    - *Contributors* are the caller-marked anchors when there are any (the appearance of their
      current settings: "anchors only with explicit user selection"). Otherwise they are the
      members that are not manual; a member is manual when every searched control is locked. A
      manual member that is not marked as an anchor neither contributes nor moves.
    - *Lightness*: the median of the contributors' median lightness.
    - *Colour* (u'v'): only when the group spans more than one calibration domain, the
      component-wise median.
    - *Camera white balance*: per domain with two or more members (or an anchor), the median of
      that domain's contributors' log camera multipliers. It is never computed across domains.
    - Anchors keep their current settings and receive no target.
    - The confidence a white-balance move towards the target carries is 1 for anchors, 0.6 for a
      camera target of three or more contributors, and 0.5 otherwise.

14. **The consistency term** is a soft term of the objective of every candidate ranked against a
    target:

        weight x ( ((|dL| - 0.01) / 0.04)^2 + ((|du'v'| - 0.002) / 0.004)^2
                   + ((|d log gains| - 0.005) / 0.015)^2 )

    Each part counts only above its dead zone (`max(0, .)`), and the colour and camera parts only
    when the target carries them. The weight is 1. The term is part of `ObjectiveTerms::total`, so
    feasibility (hard checks) still ranks first, and locked controls are never search coordinates:
    consistency cannot override either. A final term above 1 adds the review flag
    `ConsistencyLimited` (12): the group's look was not reached within the checks, locks and
    restraint.

15. **Regularisation and revalidation** (`AutoSession::regularise`). If the independent choice's
    term is already 0, the independent result stands; otherwise:
    - every evaluated candidate is re-ranked with the term (no render);
    - the stage's white-balance axis runs from the best candidate's gains to the consistency gains.
      Within the member's own domain these are the domain target's multipliers relative to as shot;
      across domains they are the gains that move the rendered u'v' to the target, solved from two
      0.02-log probe renders (camera gains are never compared across domains);
    - seeds: the exposure that reaches the target lightness, with and without the white-balance
      move, and the white-balance move alone;
    - coordinate steps at half the phase-1 sizes, for at most two passes and 16 new proxy renders;
    - recheck of the leaders, the baseline and the independent winner at analysis resolution and on
      the native crops (the revalidation);
    - confidence restraint as in phase 1, except that a white-balance residual is governed by the
      larger of the image's evidence and the target's confidence.

    The result carries a `ConsistencyRecord`: the target, the independent choice and its term, the
    chosen settings' term and objective total, the adjustment relative to the independent choice
    (EV, gain ratios, ...), the stage's renders and its white-balance axis. Each call starts from the
    independent result.

16. **The flow** (`develop_related_files`).
    - Probe every file's capture time.
    - Pass 1: analyse every file, keeping the sessions of items that can be grouped (a caller group
      of two or more, or another item within 60 s) within a 1.5 GiB budget.
    - Group, then compute the targets.
    - Pass 2: regularise every targeted member on its kept session, or on a session reopened from
      the file; by determinism the result is the same either way.

    A file that fails is recorded and the others continue; Cancelled stops the flow. Results carry
    `AutoVersions::consistency_version` (1).

17. **Recheck speed-ups.** Each is exact or schedule-independent, so results stay deterministic.
    - *Halo-free native crops.* Every crop measurement reads only the evaluation region plus 34 px
      (`NativeCrop::measured`), and stage two is per pixel except for the local-tone guided filter.
      So settings without local tone, the baseline included, are rendered on that rectangle alone
      instead of the crop with the 2r halo: 324 instead of 836 px square on a 45 MP file. Edge
      positions are now stored relative to the evaluation region, and bilinear sampling takes its
      integer and fractional parts from that relative position, so a render of either rectangle
      measures **bit-identically** (tested at an interior and a border position). This changed
      phase-1 halo values in the last bits only; the five smoke fixtures choose the same candidates
      and settings as in phase 1.
    - *Lightness only where it is read.* `measure_crop` used to convert the whole crop (with its
      halo) to lightness on one thread; it now converts only the measured region.
    - *Concurrent crops.* The four crop renders and measurements of a candidate run at once. Each
      is an independent render written to its own slot, so only the time changes.
    - *Session reuse in the consistency stage.* No decode, stage one or proxy is redone, already
      evaluated candidates are re-ranked without rendering, and settings the independent search
      already rendered are not rendered again (the stage needed 0-16 new proxy renders per member on
      the fixtures).

18. **Evaluation tool** `lucent-auto` (developer tool, not shipped; `apps/lucent-auto`):

        lucent-auto [--group-by time|none] [--out <dir>] [--tier smoke|all] <files/folders...>

    `--max-gap`, `--preview` and `--seed` are optional. The tool runs the flow above and writes:
    - per-item `lucent.auto-item` JSON: settings with the canonical recipe and its SHA-256,
      confidence and reasons, flags, diagnostics, proposals, the consistency record, every
      candidate, timing and memory;
    - `run.json`: grouping decisions and totals;
    - `summary.tsv`;
    - sRGB JPEG previews of the as-shot and automatic settings, rendered from the 1536-px analysis
      image (the interactive path the optimiser judged, not the export render), plus side-by-side
      pairs;
    - `sheet.html`, a self-contained page (images embedded) with both renders side by side, the
      settings and the flags, by group;
    - `blind/sheet.html`, the **blind** variant: per image the two renders shown as A and B in an
      order drawn by splitmix64 from a fixed seed, nothing else that distinguishes them, and a form
      that exports `lucent.blind-choices` JSON;
    - `blind/key.json` (`lucent.blind-key`), which holds the key separately.

    Safety rules:
    - The output directory must be empty and may not be, or lie inside, a source folder or the
      folder of an input file (compared as resolved, case-insensitive paths). Nothing is ever
      written into the source folders or overwritten; files are published through a temporary
      sibling and a rename.
    - `--tier` reads `fixtures/manifest.json` and selects smoke or all fixtures.
    - Multi-image files are analysed for image 0.

### Measurements (phase 2)

Same machine and build as phase 1 (Ryzen 5 5500U, `dev-core`), 2026-10-04, with another agent
building on the machine. *Before* is the phase-1 binary, saved before any phase-2 change; *after*
is this phase's. The two were run alternately per fixture, three rounds, median of the first
analysis of each run (`lucent_analysis_fixture_tests`, smoke tier). Chosen settings, candidate
indices and render counts were identical before and after for all five.

| Smoke fixture | Total, before / after | Recheck, before / after | Search, before / after |
|---|---|---|---|
| Canon EOS R5 CR3 (45 MP) | 2.67 / 2.32 s | 0.69 / 0.41 s (-41 %) | 0.37 / 0.47 s |
| Sony ILCE-7M4 ARW | 1.65 / 1.50 s | 0.40 / 0.22 s (-45 %) | 0.31 / 0.34 s |
| DJI FC4382 DNG | 1.86 / 1.75 s | 0.54 / 0.39 s (-28 %) | 0.46 / 0.45 s |
| Fujifilm X-T4 RAF | 3.14 / 2.85 s | 0.61 / 0.38 s (-38 %) | 0.38 / 0.38 s |
| Nikon Z 6 NEF | 2.01 / 1.80 s | 0.31 / 0.18 s (-42 %) | 0.38 / 0.36 s |
| Smoke tier, sum | 11.33 / 10.22 s (-10 %) | 2.55 / 1.58 s (-38 %) | 1.90 / 2.00 s |

The totals are dominated by decode (2.98 / 3.07 s of the sums) and machine noise; the recheck is
the part this phase changed. The search is unchanged code apart from the mid-tone sums, and its
spread is noise (the R5's ran 0.36-0.47 s across the rounds). On the R5 the leaders have local tone
(shadows +0.66 EV), so its gain comes from the measured-region lightness and the concurrent crops;
the baseline's crop renders (halo-free) dropped its baseline step from 0.19 to 0.16 s.

Consistency stage (from the full run below). With the session kept, regularising a member that
moved cost 0.38-0.70 s and 16 new proxy renders (the stage budget); the six members already within
the dead zones cost under 0.01 s and no render. A reopened session would cost the file's whole
first pass again, decode included (1.6-4.3 s for the grouped files of this run). The capture-time
probe lets pass 1 keep only groupable sessions: no session was reopened, and the sessions kept at
once peaked at 0.96 GiB. An earlier version that kept sessions in input order reopened 6 and peaked
at 1.51 GiB on the same run.

Full run over the fixture cache (`lucent-auto --tier all --out build\w8-p2\auto-all
fixtures\cache`: 34 files, 90 s including previews and reports, with another agent building;
per-image first pass median 1.95 s, p95 3.72 s, maximum 7.95 s for the 51 MP Fujifilm GFX 50S II).
The settings, groups and terms were identical to an earlier run of the same code under heavier load
(124 s).
- Two files failed as expected: Nikon Z 8 HE and HE\*, which LibRaw 0.22.2 cannot decode. The
  other 32 were analysed.
- 336 of the 504 native-crop renders were halo-free.
- Four related groups formed: Canon 5D Mark IV RAW + sRAW (29 s apart), Nikon Z 6 pair (8 s),
  Fujifilm X-T4 pair (15 s), and four Sony ILCE-7M4 frames (gaps 28, 44, 24 s).
- Correctly split: the Sony uncompressed frame (71 s before the group), the Sony APS-C frame
  (210 s after), and the four Canon R5 frames (gaps of 58-297 s, and full-frame versus 1.6x-crop
  layouts differing by 0.95-1.01 EV per cell).
- Regularisation reduced the X-T4 pair's term from 0.75 to 0.25 and 0.36: their median lightness
  went from 38.9 and 33.9 L* (5.0 apart) to 37.6 and 34.1 L* (3.5 apart; target 36.4): -0.09 EV
  of exposure and -0.06 EV of shadows on the brighter frame, +0.03 EV of shadows on the darker one,
  and each frame's camera blue gain moved 0.4 % towards the other's. The darker frame did not
  follow further although every candidate stayed feasible: +0.1 EV or more raised its highlight
  term by about 0.47 (the white anemones approach clipping), more than the consistency term fell,
  and the full camera white-balance move cost 0.70 in the white-balance plausibility term (weak
  evidence). This is the soft penalty working as specified.
- The Sony group's terms fell from 0.07-0.10 to 0.01-0.02 (one frame -0.05 EV, another 0.2 %
  blue). The 5D IV and Z 6 pairs were already within the dead zones and kept their independent
  results.
- No member was flagged `ConsistencyLimited`. Within each group the as-shot whites agreed within
  0.0018 uv. The camera white-balance axes ended at most 0.4 % (red) and 1.6 % (blue) from the
  start; the chosen settings moved a gain by at most 0.4 %.

Visual inspection of the side-by-side pairs (`pairs/`, as shot left, automatic right), by the
author of this phase:
- R5 still life (raw, +1.5 EV, shadows +0.66 EV): the fruit and vegetables are well exposed and
  pleasant, and the lemon keeps its texture. But the near-black table and blue-grey cloth are lifted
  to a dull grey, so the picture loses its low-key mood and depth. It is plausible as a "correct
  exposure", but a photographer would probably add contrast back.
- R5 crop frame (+1.5 EV, shadows +0.63, highlights -0.31): the least natural of the set. The
  three moves together compress the tonal range into a flat, slightly chalky look (the potatoes
  pale, the backdrop grey). Nothing is clipped or haloed, but it reads as over-processed.
- X-T4 pair (colour checker in anemones): both automatic renders are brighter and now look alike,
  which is the consistency stage's purpose, and the checker's neutral patches stay neutral. Both are
  flatter than the as-shot renders: the dark foliage turns greyish green. The -0.25 EV highlight
  compression on the compressed frame dulls the white anemones slightly.
- 7D Mark II ducks on an overcast pond (+0.61 EV, shadows +0.19): the water and birds are lighter
  and a little washed out. Natural enough, but the darker as-shot mood was arguably intended. It is
  flagged for visible shadow noise.
- Tungsten bathroom (linear DNG): a small cooling (R x0.987, B x1.034) that keeps the warm light.
  Natural; a good example of a restrained white-balance residual.
- DJI FC4382 castle landscape (+0.25 EV, highlights -0.12): slightly brighter forest, the clouds
  kept. Natural, hard to tell apart without flipping.
- Leica M Monochrom (+0.54 EV): a brighter overcast street scene with highlights intact. Natural.
- Sony 7M4 group (compressed frame, -0.05 EV, blue x1.002): indistinguishable from as shot, which is
  correct for an already well-exposed frame.

Overall, the results with small edits (most of the cache: 22 of 32 within 0.15 EV with no local
tone) look like plausible, mild corrections, and nothing is garish, clipped or haloed. White
balance is almost never changed (the estimators' disagreement restrains it, phase 1's known limit).
The systematic weakness is in the large edits: brightening by the full +1.5 EV and lifting shadows
flattens deliberately dark surroundings and overcast scenes, and adding highlight compression on
top gives a flat, lifted look. That is a calibration question for the brightness band, the
low-key detection and the shadow-lift proposal (phase 1's "uncalibrated" list), not a consistency
one. The blind sheet exists to measure exactly this.

### Consequences (phase 2)

- Related shots are regularised softly and revalidated per image. Locks and hard checks are
  untouched by construction (tested: a locked exposure never moves; a target that only darkening
  could reach makes the most consistent candidates block shadows, they lose, and the result is
  flagged). Determinism holds across kept and reopened sessions (tested on synthetic scenes and on
  the Z 6 pair).
- Brightness consistency acts broadly. White-balance consistency acts within a domain when the
  members' camera whites differ beyond 0.5 % and their evidence permits. Across cameras it acts
  through the two probe renders, and the per-image white-balance and gamut terms limit how far.
- Shadow-only edits move the median lightness only when the median lies in the shadows: a member
  with exposure locked can rarely follow a lightness target with shadows alone, and the term stays
  (flagged `ConsistencyLimited`, as tested).
- Conservative grouping leaves sequences apart when gaps exceed 60 s or the framing changes (crop
  modes). Callers can override with group ids. The CLI does not expose overrides or anchors yet; the
  library and its tests do.
- The time-based grouping relies on camera clocks. Two cameras with unsynchronised clocks will
  rarely group, which is safe but conservative.
- `ConsistencyRecord`, the shot features and the grouping decisions are what W7's job journal
  should persist with each item's auto result, together with `consistency_version`. The
  application needs "keep related photos consistent" (blueprint section 2), anchor marking and a
  revision note for regularised settings.
- The blind sheet is groundwork only. The section 10 gate still needs the fixed preset and
  LUT-only arms, photographers, the correction-effort protocol and a corpus.

### What is uncalibrated (phase 2)

The grouping thresholds (gap, span, similarity and illumination limits), the term's dead zones,
scales and weight, the target confidences, the stage budget and steps, and the session budget are
engineering defaults like phase 1's and are calibrated the same way. Grouping additionally needs
annotated sequences: shots a photographer considers one series, and changes of light. The
consistency term needs side-by-side preference data: pairs of related renders judged "consistent
enough".

### Alternatives considered (phase 2)

- **One shared exposure (or settings) for a group.** Forbidden by the blueprint, and wrong for
  related shots of different framing and metering.
- **Averaging camera multipliers across cameras.** Forbidden by the blueprint, and meaningless:
  multipliers are in each sensor's own space. The colour target is a colour-managed u'v' instead.
- **Single linkage on the light** (comparing each shot only with the previous one). It lets light
  drift through a slow sequence; the complete linkage costs one comparison per member.
- **Regularising inside the first pass** (sequentially, towards the members seen so far). That
  makes results depend on input order; the two-pass flow with a median target does not.
- **Re-decoding every member for pass 2.** It doubles the decode and stage one for grouped files;
  the session costs 50-90 MiB per kept file, bounded by a budget, with reopening as the fallback.
- **CIELAB a\*b\* means for colour consistency.** They scale with lightness, so an exposure
  difference would read as a colour difference; u'v' of summed XYZ does not.
- **Rendering crops for the halo-free path from the halo crop at measurement time.** It would save
  the extra 1-1.3 MB per crop of memory, but it needs a second copy per render. Cutting both crops
  once is simpler and the memory is small.
