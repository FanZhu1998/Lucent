# ADR 0011: Natural v1 automatic settings: metric contract, proposals, bounded search and confidence

- **Status:** Accepted
- **Date:** 2026-10-03
- **Workflow:** W8 (Natural v1 optimiser), phase 1 of 3 (core; the application's "Auto" and
  batch flows are wired once W7's worker lands)

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
  throughput needs them.
- Needs recorded for other modules (phase 2): an application `RevisionOrigin` for automatic settings
  and a note carrying the policy and search versions with each auto revision; the job journal (W7)
  should persist `AutoResult`'s flags, confidence and versions; the UI's "Auto" runs
  `auto_develop_file` in the worker with the item's current settings (locks included).
- Related-shot consistency (blueprint section 6) and learned proposals are not part of this phase.

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
