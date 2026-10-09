# ADR 0019: Learned appearance research: reproduce Image-Adaptive-3DLUT, keep its weights and data out of Lucent, and implement the method natively

- **Status:** Accepted (2026-10-09): native C++ inference instead of ONNX Runtime, and the rights position. Product integration is deferred until a model with clear redistribution rights exists; the deterministic Natural v1 optimiser remains the production baseline.
- **Date:** 2026-10-09
- **Workflow:** W11 (Learned appearance research), phase 1: reproduction, rights and the native plan
  (blueprint Appendix D2 steps 1-5; steps 6 and 7 are later phases)
- **Would amend, if accepted:** the blueprint's section 1 "Inference" decision for this model (a native
  implementation instead of ONNX Runtime, decision 5) and the W11 roadmap item "ONNX parity"
  (decision 9)

## Context

The blueprint selects Image-Adaptive-3DLUT as "an optional, validated proposal/appearance stage"
(section 1, *Learned appearance*), with "ONNX Runtime CPU deployment where reference parity is
demonstrated" (section 1, *Inference*). Section 5 restricts the sRGB checkpoint to "a **rendered
sRGB-to-sRGB** method", never connected to camera RGB, exposed "only inside an explicit sRGB
appearance route", and requires a model manifest. Section 6 infers the LUT once from the chosen
upstream rendering, offers "0, 0.25, 0.5, or 0.75 strength after domain checks" with identity
eligible, and freezes "LUT or its reproducible assets" in the recipe. Section 10's gate: a learned
method is promoted only if it "improves a prespecified held-out quality criterion without exceeding
a prespecified defect budget; otherwise remain optional or disabled". Appendix C records the code as
Apache-2.0 and says to "separately resolve model/data use"; Appendix D4 says FiveK permission "did
not establish permission to commercially train on or redistribute its images or derived weights".

This phase did the work in `research/lut` (README there; everything is reproducible with
`research\lut\tools\setup.ps1`, `build_trilinear_ext.ps1` and `run_all.ps1`). Facts established on
2026-10-09 follow.

**Reproduction.** The snapshot `b491f6df64a588864739a157db271e5c848e1805` was checked out unmodified
(gitignored, verified byte-identical to its blobs). A separate hash-pinned venv
(`research/lut/.venv`: CPython 3.12.10, CPU-only torch 2.14.1 and torchvision 0.29.1 from the
official CPU index) ran upstream's own `demo_eval.py`. Only the README's two documented PyTorch 1.x
substitutions were applied, plus `map_location="cpu"`, because both checkpoints hold `cuda:0`
storages. The XYZ-only imports were replaced by stubs that raise on use. The repository's CPU
extension (`trilinear_cpp/src/trilinear.cpp`, unmodified) was built by its own `setup.py` with MSVC
14.44.

- The sRGB checkpoint loads (`weights_only=True`, strict keys) and runs. For the bundled demo input
  `a1629.jpg` (1620 x 1080) the weights are `[1.9081297, -0.3463063, -0.9284921]`, used without a
  softmax. The float32 output hashes to `bc968771...cff73` and the 8-bit output to `a68b5513...42caf`
  (`research/lut/results/demo_reproduction.json`).
- The output of the extension, an independent NumPy mirror of its C++ loop, and a pure-PyTorch
  sampler is **bit-identical**, on both bundled sRGB examples and on all 16,777,216 8-bit colours
  through three LUTs (demo, seeded random, identity: 0 of 150,994,944 samples differ). It is also
  bit-identical on 163 float edge cases, and in full frame against 256 x 256 tiles and single rows
  (`results/trilinear_parity.json`).
- An independent float64 NumPy classifier matches PyTorch's weights within 3.4e-6; outputs from
  its LUT differ by at most one 8-bit code value, in 409 of 5,248,800 samples.
- PyTorch's classifier is deterministic per thread count, but 1 thread differs from 2 or 6 threads
  by up to 2.4e-7 in the weights (6 output samples change by one code value).
- The classifier resizes its input to 256 x 256 bilinearly without antialiasing, so the weights
  depend on the resolution given. A 720 x 480 copy (the 480p training scale) instead of 1620 x 1080
  moves outputs by up to two code values.
- The demo LUT brightens strongly. It clips 2.3 % of samples to black (2.9 % for the second
  example), and maps neutral inputs above about 0.72 above 1. 29 % of its entries exceed 1.
- `models.py` (the README's default PyTorch 0.4.1 path, most likely the training-time arithmetic)
  passes `binsize = 1.0001/32`; `models_x.py` passes `1.000001/32`. With the same LUT the difference
  changes 1.3 % of demo samples by one code value.
- The reference arithmetic takes the cell index from `floor(x / binsize)` but the fraction from
  `fmod(x, binsize) / binsize`. Across every float32 in [0, 1], 8 inputs (each the largest float32
  below a cell boundary) make the two disagree: the reference then interpolates one cell too high
  and jumps one LUT node.
  - No 8-bit or 16-bit code value does this, but an unquantised float32 image does: 3 of 12.6
    million random samples, with output errors up to 0.073 for the demo LUT.
  - Taking the fraction from the exact remainder `(x - i * binsize) / binsize` (in double, clamped
    to [0, 1]) removes the jumps and is otherwise **bit-identical** to the reference. That covers
    every 8-bit colour for all three LUTs, and every random float32 sample except those at the
    eight inputs.
  - The inputs must also lie in [0, 1.0000008], or the C++ code reads outside the LUT.
- The architecture: bilinear resize to 256 x 256, five 3 x 3 stride-2 convolutions (16, 32, 64, 128,
  128 channels) with LeakyReLU(0.2), InstanceNorm2d (affine, eps 1e-5, per-image statistics) after
  the first four, Dropout (identity in eval), and an 8 x 8 convolution to 3 weights. That is
  270,083 parameters and 73,162,752 MACs (146.3 MFLOP, confirmed by `torch.utils.flop_counter`;
  about 152.6 MFLOP in all), a fixed cost per image. The basis is 3 LUTs of 3 x 33^3 float32 in
  layout `[c][b][g][r]` (323,433 parameters, 1.29 MB), 593,516 parameters in all. Application costs
  76 FLOPs and 24 LUT loads per pixel; the unmodified extension ran 15.5 Mpx/s on one core of the
  Ryzen 5 5500U (`results/model_summary.json`).

**Rights**, from primary sources retrieved on 2026-10-09:

- **Code.** `LICENSE` is Apache-2.0 (blob `261eeb9e`, SHA-256 `c71d239d...0ab4`; GitHub's licence API
  reports `Apache-2.0`). It was added on 2021-09-23 (commit `9b59e5d`, PR #53), its appendix copyright
  line is the unfilled template "Copyright [yyyy] [name of copyright owner]", and there is no NOTICE
  file and no per-file header. Section 4 requires, for redistribution, that "You must give any other
  recipients of the Work or Derivative Works a copy of this License", that "You must cause any
  modified files to carry prominent notices stating that You changed the files", and that notices
  are retained. Section 3's patent licence covers only "patent claims licensable by such Contributor
  that are necessarily infringed by their Contribution(s)". Not every file is Apache-2.0: `ssim.m`
  says "Permission to use, copy, or modify this software ... for educational and research purposes
  only ... shall not be used, rewritten, or adapted as the basis of a commercial software or hardware
  product without first obtaining permission".
- **Checkpoints.** `pretrained_models/sRGB/LUTs.pth` (`c1bb2bc4...821a`) and `classifier.pth`
  (`bae98653...8035021`) were committed on 2020-09-28 (`05d5748`, "Add files via upload"), a year
  before the licence. The README says nothing about their terms. The paper trains on MIT-Adobe
  FiveK: "the images retouched by expert C are used as the groundtruth", "4,500 image pairs are used
  for training", short side 480 px ([arXiv 2009.14468](https://arxiv.org/abs/2009.14468), arXiv
  non-exclusive distribution licence).
- **MIT-Adobe FiveK.** The [dataset page](https://data.csail.mit.edu/graphics/fivek/) says "You can
  use these photos for research under the terms of the following licenses". Both
  [LicenseAdobe.txt](https://data.csail.mit.edu/graphics/fivek/legal/LicenseAdobe.txt) (SHA-256
  `8121ecfc...84fcf`, 2,690 files) and
  [LicenseAdobeMIT.txt](https://data.csail.mit.edu/graphics/fivek/legal/LicenseAdobeMIT.txt)
  (`69f73c48...182af3d`, 2,310 files), both dated 2011-06-18, are a "RESEARCH LICENSE".
  - They grant "to use, copy, modify, merge, publish, distribute and/or sublicense copies of the
    Images" subject to: "1. You may only exercise these rights granted to you solely for your own
    research purposes, and you shall not exercise any of these rights in any manner that is intended
    for or directed toward commercial advantage or monetary compensation. 2. The above copyright
    notice and this license shall be included in all copies or substantial portions of the Images".
  - They also forbid endorsement, and "terminate automatically upon any breach".
  - Both bundled sRGB examples are FiveK images: `a1629` is `a1629-JI2E5331` (filesAdobeMIT.txt,
    line 1785) and `a1509` is `a1509-dvf_034` (filesAdobe.txt, line 927).
- **Patents.** A basic web search found no patent on the paper's method. It did find US 12,125,182 B2
  (BOE Technology; priority 2021-04-27; active, anticipated expiry 2041-04-27), whose claim 1 covers
  a CNN that produces N weights for N preset 3D LUTs in SDR-to-HDR conversion, followed by a
  refinement CNN. That is adjacent to this method, not identical to Lucent's proposed use. One author
  is with DJI's Camera Group. No professional search or legal review has been done.

**ONNX Runtime**, from primary sources retrieved on 2026-10-09:

- **Release.** v1.30.0 (2026-09-10) is MIT-licensed. Its Windows x64 CPU package holds
  `onnxruntime.dll` at 16.46 MB plus a 0.34 MB `ThirdPartyNotices.txt`.
- **vcpkg.** At Lucent's vcpkg baseline (`e143402b`) the `onnxruntime` port is 1.23.2, a source
  build with 18 library dependencies (abseil, protobuf, re2, onnx, eigen3, flatbuffers and others).
- **Kernel selection.** Its MLAS kernels are selected at run time from CPUID: SSE, AVX, FMA3 or
  AVX512F GEMM and convolution kernels (`onnxruntime/core/mlas/lib/platform.cpp` at v1.30.0).
- **Determinism.** The Python API documents `use_deterministic_compute` as "Whether to use
  deterministic compute. Default is false."
- **The LUT pass.** It is not a standard ONNX operator in this form. `GridSample` has different
  coordinate rules, so the full-resolution pass would stay native in any case.

## Decision

1. **Research boundary.** `research/lut` (venv, pins, tools, tests and results) and the gitignored
   `research/third_party` checkout are research only.
   - Nothing in `src/`, `apps/` or `installer/` imports from them, links them, or bundles any file
     from the checkout.
   - The committed results hold JSON and rights-clean PNGs only: a generated chart and LUT curves.
   - Previews of the FiveK photographs stay in the gitignored `research/lut/build`.

2. **The reference.** "The reference" for Lucent's work is the upstream paired sRGB model at
   `b491f6d`, reached through the README's PyTorch 1.x path: `models_x.py`, `binsize = 1.000001/32`,
   the `trilinear_cpp` CPU arithmetic and the unnormalised weight combination. It is identified by
   the hashes in `research/lut/results/demo_reproduction.json`. The legacy `binsize = 1.0001/32` is
   recorded as a sensitivity (up to one code value), not as a second reference.

3. **Rights.**
   - **(a) Code: may be used.** Lucent may implement the method. Its native code is written from the
     executable specification in `research/lut/tools/trilinear_numpy.py` and `classifier_numpy.py`.
     Because the kernel follows upstream's layout and arithmetic, Lucent's third-party notices credit
     Image-Adaptive-3DLUT under Apache-2.0 and include the licence text. Files under other terms
     (`ssim.m`) are not used.
   - **(b) Upstream weights: research only.** All of upstream's checkpoints (sRGB and XYZ, paired and
     unpaired) are research only. They are not shipped, not converted (to ONNX or C++ arrays), not
     downloaded at run time, and not used to produce shipped assets or committed product test
     fixtures.
   - **(c) MIT-Adobe FiveK: research only, under its licences.** It is not used to train, tune or
     select a shipped model, and is not shipped or committed in fixtures or documentation. Research
     copies keep the licence with them.
   - **(d) Patents: not established.** A freedom-to-operate review precedes shipping any
     learned-LUT feature. It covers at least the image-adaptive LUT method and adjacent patents such
     as US 12,125,182 B2.
   - **(e) Consequence.** A shipped learned appearance needs weights trained by or for Lucent on
     photographs whose terms allow that use, or written permission from Adobe, MIT and the authors,
     recorded in the model asset's provenance. Until then, no learned appearance ships.

4. **Native numerical contract** (blueprint section 5; the specification a later phase implements).
   - **(a) LUT application.** Float32 throughout.
     - The LUT is `[c][b][g][r]` with D = 33. Input is sRGB-encoded RGB, clamped to [0, 1]; a
       non-finite input is an error, never a lookup.
     - `binsize = float(1.000001 / (D - 1))`; the cell index is `i = floor(x / binsize)` with
       float32 division, as upstream.
     - The fraction is `f = clamp((x - i * binsize) / binsize, 0, 1)`: the remainder and the
       division in double, the result rounded to float32. This is upstream's `fmod` fraction bit
       for bit wherever upstream's index is correct, and the continuous answer at the 8 float32
       inputs where it is not.
     - The quotient fraction `x / binsize - i` is not used: it is continuous but up to 4.3e-6 from
       the reference.
     - The eight weights and the per-channel sum are evaluated left to right without contraction
       (no `/fp:fast`, no FMA).
     - The pass is per pixel, so it may be parallelised over pixels, and tiling cannot change
       results.
   - **(b) Predictor.** A native forward pass of the classifier as specified above:
     - the bilinear resize uses PyTorch's half-pixel rule (`align_corners=False`), no antialiasing;
     - InstanceNorm uses biased variance;
     - the output is three unconstrained weights, with no softmax;
     - reductions use a fixed order and double accumulators, and parallelism, if any, is over
       independent outputs only, so results do not depend on the thread count or the CPU's
       instruction set.
   - **(c) Fusion and strength.** `LUT = w0*L0 + w1*L1 + w2*L2` in float32, left to right. Strength
     s in {0.25, 0.5, 0.75} is `LUT_s = (1 - s) * I + s * LUT`, where I is the exact k/32 grid; by
     linearity this equals blending the outputs. Strength 0 removes the stage rather than applying
     I, which maps x to x / 1.000001.
   - **(d) Classifier input.** The oriented analysis image is reduced with an area filter to a short
     side of 480 px (the training scale), then the model's own 256 x 256 resize. The size and filter
     are part of the model's input identity.
   - **(e) Recipe.**
     - The recipe stores the model asset id, version and content hash, the classifier-input
       identity, the three weights as exact float32 bit patterns, and the strength.
     - Replay rebuilds the fused LUT bit-exactly from these (12 bytes of weights instead of a
       431 KB LUT) and never re-runs the classifier.
     - Any upstream change invalidates them (blueprint section 6, steps 5 and 6).
   - **(f) Tolerances** (from the measured numbers):
     - The native kernel is **bit-exact** against the research implementations
       (`trilinear_numpy.forward_exact_remainder`, equal to upstream's extension away from the
       eight inputs). This holds over the exhaustive 8-bit cube and over seeded random float32
       inputs, for synthetic LUTs (identity, seeded random).
     - At the eight census inputs, the kernel is within 1e-4 of the float64 ideal.
     - The predictor is within 2e-5 absolute of PyTorch's weights on a fixed, rights-clean image
       set.
     - Identity, ramp, corner, midpoint and tile checks as in `trilinear_parity.json`.
     - No NaN or Inf.

5. **Recommended integration path: native C++, not ONNX Runtime, for this model.**

   | | Native C++ (recommended) | ONNX Runtime 1.30.0 CPU |
   |---|---|---|
   | New dependency | None | `onnxruntime.dll` 16.46 MB, notices 0.34 MB; vcpkg 1.23.2 is a source build with 18 dependencies |
   | Licence | Lucent code plus an Apache-2.0 attribution | MIT, plus its third-party notices |
   | Scope | About 500 lines: resize, 3 x 3 stride-2 and 8 x 8 convolutions, InstanceNorm, LeakyReLU, fusion, LUT kernel | Classifier only; the LUT pass, fusion and strength stay native anyway; adds an export toolchain, opset and IR versions |
   | Determinism | Fixed order, `/fp:precise`, no contraction: the same weights on every x64 CPU and at any thread count | CPUID-selected MLAS kernels (SSE, AVX, FMA3, AVX512F); deterministic compute off by default; graph optimisations change the numerics |
   | Parity evidence | A float64 port already matches within 3.4e-6 (at most one code value) | Unproven; export is Appendix D2 step 6 |
   | Cost | 73 MMAC once per image: an estimated 30-100 ms single-threaded (the BLAS-backed float64 NumPy port takes 0.1 s) | Not measured; likely close to PyTorch's oneDNN (4-22 ms here); irrelevant at once per image |

   ONNX Runtime remains the blueprint's default for a future model whose size or operator set
   justifies a general runtime. That choice needs its own ADR.

6. **Model manifest.** When a rights-cleared model exists, it ships as an ADR 0018 asset.
   - Its manifest records the blueprint section 5 fields, as drafted for the upstream model in
     `results/model_summary.json` (`manifest_fields_draft`), plus the training-data provenance and
     its terms.
   - A missing or incompatible model selects Natural v1 (no learned appearance) for new recipes.
     Replay of a recipe that used it reports the missing dependency.

7. **Domain checks before a strength is offered.**
   - The image is on the explicit sRGB appearance route, never camera RGB or the wide-gamut TIFF
     route.
   - The classifier input identity matches the manifest.
   - The weights and the fused LUT are finite and within the manifest's declared bounds.
   - The candidate is then measured by the ADR 0011 metric contract against the baseline: newly
     clipped samples (the upstream LUT clips 2-3 % to black), neutral drift, and tone reversals in
     the visible range. Identity is always eligible.

8. **Promotion gate** (blueprint section 10). Learned appearance stays disabled, with no user-visible
   control, until all of the following pass:
   - **G1 Rights:** decision 3(e) and the freedom-to-operate review.
   - **G2 Numerics:** decision 4(f) and a verified manifest.
   - **G3 Domain:** decision 7.
   - **G4 Quality:** a held-out criterion fixed before testing improves. That is blind preference
     and correction effort against Natural v1, the shared preset and LUT-only, on a corpus split by
     camera model and trip or scene, including held-out camera models.
   - **G5 Defects:** a defect budget fixed before testing is not exceeded at the chosen strength.
     It covers induced clipping, neutral drift, tone reversals, hue shifts and halos.

   The upstream checkpoint can never pass G1. It is a research reference only.

9. **Roadmap.** If this ADR is accepted, the W11 roadmap item "ONNX parity" becomes "native predictor
   parity", and the remaining W11 phases are:
   - (phase 2) the native kernel and predictor with the 4(f) parity tests;
   - (phase 3) rights-cleared training data and a Lucent-trained model, evaluated against G4 and G5.

## Consequences

- **Gains.** No inference runtime enters the product for a 270K-parameter network, so there is no
  16.5 MB DLL, no export toolchain, and no CPU-dependent results. Recipes stay replayable from 12
  bytes of weights.
- **Executable specification.** The research tools become the specification and the source of
  parity tests. The rights exposure is contained: no FiveK pixels or FiveK-derived weights reach the
  product or the repository's committed files.
- **Costs.** Lucent owns about 500 lines of numerical code and its tests. More importantly, **no
  learned appearance can ship until Lucent has rights-cleared training data and its own trained
  model**, which is a data-acquisition and training project, not an integration task. The automatic
  path therefore continues on Natural v1 (blueprint section 1: "The release must work automatically
  even if learned components do not pass quality or redistribution gates").
- **Blueprint deviation.** The section 1 *Inference* decision no longer applies to this model, and
  W11's roadmap entry changes (decision 9). Both need the user's acceptance.
- **Open technical points.**
  - Whether a Lucent-trained model keeps binsize `1.000001/32` or moves to the exact grid; that is a
    training-time choice, and the kernel takes binsize from the manifest.
  - Which analysis resolution and filter to fix (decision 4(d) is a proposal backed by one
    sensitivity measurement on two images).
  - How strength interacts with the metric contract's calibration.
- **Revisit if:**
  - Adobe, MIT and the authors grant written permission for the upstream weights;
  - a substantially larger model is adopted;
  - ONNX Runtime becomes a product dependency for another reason;
  - the freedom-to-operate review finds a blocking patent.

## Alternatives considered

- **ONNX Runtime for the classifier** (the blueprint's default). Not chosen for this model. It adds a
  16.5 MB DLL, notices and an export pipeline for a network whose whole forward pass is about 500
  lines. Its CPU kernels vary by instruction set, which weakens cross-machine reproducibility. The
  LUT pass would remain native anyway.
- **Shipping the upstream checkpoint.** Rejected: its terms are unstated and its training data is
  licensed "solely for your own research purposes".
- **Retraining on FiveK.** Rejected for the same licence, which also covers derived uses "directed
  toward commercial advantage".
- **Reproducing upstream's fmod arithmetic in production.** Rejected. It jumps one LUT node at 8
  float32 inputs in [0, 1], and Lucent's float pipeline is not quantised before the LUT; random
  float32 data hits those inputs about once per 4 million samples.
- **Taking the fraction from the quotient** (`x / binsize - floor(...)`). Rejected: it is
  continuous, but it is not bit-compatible with the reference. It is up to 4.3e-6 away on steep
  LUTs and changes 303 of 50 million demo-LUT 8-bit samples by one code value. The exact remainder
  (decision 4(a)) fixes the jumps and is bit-identical everywhere else.
- **`grid_sample` (upstream issue 14).** Not the trained arithmetic: different coordinate rules.
  Usable only for training Lucent's own model, with deployment arithmetic matched to training.
- **Storing the fused LUT in each recipe.** Not chosen: 431 KB per image instead of 12 bytes, with no
  gain, since fusion is deterministic.
- **Running the classifier on the full-resolution image**, as the demo does. Rejected: without
  antialiasing, the weights then depend on the camera's resolution. A fixed reduced input matches
  the training scale and the blueprint's fixed analysis image.
- **HDRNet or AdaInt** (blueprint Appendix A alternatives). Out of scope for this phase. They face
  the same rights questions.

## Evidence

- `research/lut/README.md`. The results: `research/lut/results/demo_reproduction.json`,
  `trilinear_parity.json` and `model_summary.json`.
- The tests: `research/lut/tests` (19 tests).
- Upstream: <https://github.com/HuiZeng/Image-Adaptive-3DLUT> at `b491f6d`, including `LICENSE`,
  `README.md`, `demo_eval.py`, `models.py`, `models_x.py`, `trilinear_cpp/` and `ssim.m`. GitHub
  licence API: <https://api.github.com/repos/HuiZeng/Image-Adaptive-3DLUT/license>.
- The paper: <https://arxiv.org/abs/2009.14468>, read via <https://ar5iv.labs.arxiv.org/html/2009.14468>.
- FiveK: <https://data.csail.mit.edu/graphics/fivek/> and `legal/LicenseAdobe.txt`,
  `legal/LicenseAdobeMIT.txt`, `legal/filesAdobe.txt`, `legal/filesAdobeMIT.txt`.
- The adjacent patent: <https://patents.google.com/patent/US12125182B2/en>.
- ONNX Runtime:
  - <https://github.com/microsoft/onnxruntime/releases/tag/v1.30.0>;
  - <https://github.com/microsoft/onnxruntime/blob/v1.30.0/onnxruntime/core/mlas/lib/platform.cpp>;
  - <https://onnxruntime.ai/docs/api/python/api_summary.html>;
  - the vcpkg port `ports/onnxruntime/vcpkg.json` at `e143402b`.
- All retrieved 2026-10-09. These are engineering findings, not legal advice.
