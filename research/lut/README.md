# Learned appearance research: Image-Adaptive-3DLUT

Reproduction of the selected learned-appearance method (blueprint Appendix A.1, Appendix D2 steps
1-5) on CPU, with the rights and native-integration analysis in
[ADR 0019](../../docs/adr/0019-learned-appearance-research.md) (Proposed).

**Research only.** Nothing here is shipped, built into Lucent, or imported by the product, and
none of it establishes a right to distribute the upstream model, its weights or its example
images. The upstream checkpoint and the MIT-Adobe FiveK images are **research only** (see
"Rights" below).

| | |
|---|---|
| Upstream | <https://github.com/HuiZeng/Image-Adaptive-3DLUT> at `b491f6df64a588864739a157db271e5c848e1805` (2022-11-26), the Appendix C snapshot; checked out unmodified in `research/third_party/Image-Adaptive-3DLUT` (gitignored) |
| Model | `pretrained_models/sRGB` (paired): `classifier.pth` + `LUTs.pth`, a rendered **sRGB-to-sRGB** method; never fed camera RGB |
| Environment | `research/lut/.venv`: CPython 3.12.10, torch 2.14.1+cpu and torchvision 0.29.1+cpu from the official CPU index, numpy 2.5.3, Pillow 12.3.0 (libjpeg-turbo 3.1.4.1), all hash-pinned in `requirements.txt` |
| Trilinear extension | `trilinear_cpp/src/trilinear.cpp`, unmodified, built by the repository's own `setup.py` (CppExtension, no CUDA) with MSVC 14.44 (`/O2 /GL /MD /std:c++20`, default `/fp:precise`, no `/arch`); only C4996 deprecation warnings |
| Status | **Reproduced on CPU**: the checkpoint loads and runs; three independent trilinear implementations agree bit for bit; see "What was and was not shown" |

## Setup

From the repository root, in PowerShell (Windows PowerShell 5.1 or PowerShell 7):

```powershell
research\lut\tools\setup.ps1              # research\lut\.venv (--require-hashes) and the pinned checkout
research\lut\tools\build_trilinear_ext.ps1  # MSVC build of the upstream CPU extension (needs VS 2022 Build Tools, Ninja)
research\lut\tools\run_all.ps1            # regenerates research\lut\results and runs the tests
```

`setup.ps1` clones with `core.autocrlf=false` and checks that every working-tree file hashes to its
committed blob (the global `autocrlf=true` would otherwise rewrite text files such as
`IdentityLUT33.txt`). It never touches `research/.venv`. Every tool verifies the checkout is
exactly the pinned commit with no local or untracked files before and after it runs (upstream
modules are imported with bytecode writing disabled, and demo outputs go to `research/lut/build`).

Tests (19, about 5 s; extension and checkout tests skip with a reason when those are absent):

```powershell
research\lut\.venv\Scripts\python.exe -m unittest discover -s research\lut\tests -v
```

## Layout

| Path | Purpose |
|---|---|
| `requirements.txt` | Exact pins with SHA-256 hashes (CPU-only torch/torchvision as direct URLs from the official CPU index). |
| `tools/setup.ps1`, `tools/build_trilinear_ext.ps1`, `tools/run_all.ps1` | Environment, extension build, full regeneration. |
| `tools/reproduce_demo.py` | Runs upstream `demo_eval.py` itself (patched copy, see "Deviations") with each trilinear backend; records hashes and the analyses. Writes `results/demo_reproduction.json` and the chart and curve PNGs. |
| `tools/trilinear_parity.py` | Bit-exact parity of the three trilinear implementations and the D2 step 5 checks. Writes `results/trilinear_parity.json`. |
| `tools/model_summary.py` | Architecture, parameters, MACs (cross-checked with `torch.utils.flop_counter`), indicative timings, draft manifest fields. Writes `results/model_summary.json`. |
| `tools/trilinear_numpy.py` | Independent NumPy mirror of the C++ CPU loop (the specification of the LUT kernel), plus the float64 ideal and two fraction variants (quotient; exact remainder, the proposed production arithmetic). |
| `tools/trilinear_torch.py` | Pure-PyTorch sampler with the extension's exact arithmetic, and a drop-in `trilinear` module (`ExtensionApi`). |
| `tools/classifier_numpy.py` | Independent float64 NumPy forward pass of the classifier (the specification of the predictor). |
| `tools/upstream_model.py`, `tools/lut_common.py` | Loading the upstream model as `demo_eval.py` does; hashing, checkout verification, environment records. |
| `tests/` | `test_trilinear.py` (synthetic parity and reference behaviour), `test_reproduction.py` (demo patch, predictor parity, committed-result regression). |
| `results/` | Committed: three JSON reports and four small PNGs (synthetic chart and LUT curves). |
| `build/` | Gitignored: the extension, the patched demo, demo outputs (`result.jpg`) and photo previews. |

Hash conventions (`tools/lut_common.py`): files are SHA-256 of their bytes; arrays are SHA-256 of
their C-contiguous little-endian bytes, recorded with dtype and shape.

## What was run

The historical command is `python demo_eval.py --input_color_space sRGB`. `reproduce_demo.py` runs
that script in-process (`runpy`, working directory = the checkout) on the bundled demo input
`demo_images/sRGB/a1629.jpg`, and on `local_tone_mapping/a1509.jpg` as a second bundled sRGB
example (that image belongs to upstream's MATLAB tone-mapping demo, not to `demo_eval.py`). Each
run is repeated with three `trilinear` backends: the built extension, the pure-PyTorch sampler and
the NumPy reference. Every call is recorded so the float32 output is captured before `demo_eval.py`
quantises it in place.

### Deviations from the historical environment (all of them)

1. **Software versions.** Python 3.12.10, torch 2.14.1+cpu, torchvision 0.29.1+cpu, numpy 2.5.3,
   Pillow 12.3.0 instead of the repository's `requirements` (torch 0.4.1, torchvision 0.2.2,
   numpy 1.19.2, PIL 6.1.0, opencv-python 3.4.3).
2. **The PyTorch 1.x path, as the README documents.** `models_x.py` with the `trilinear_cpp`
   extension instead of `models.py` with `trilinear_c`, whose `build.py` needs `torch.utils.ffi`
   (absent from modern PyTorch). The README's two substitutions are applied to a copy of
   `demo_eval.py`: `from models import *` to `from models_x import *`, and
   `result = trilinear_(LUT, img)` to `_, result = trilinear_(LUT, img)`.
   Consequence: `binsize` is `1.000001/32` (models_x) rather than `1.0001/32` (models.py). With the
   same fused LUT, the legacy value changes 69,934 of 5,248,800 demo output samples (1.3 %) by one
   8-bit code value (max float difference 1.4e-4).
3. **`map_location="cpu"`** on the demo's two `torch.load` calls (not in the README). Both sRGB
   checkpoints hold CUDA storages (`cuda:0`) and cannot be loaded on a CPU-only machine otherwise.
4. **XYZ-only imports stubbed.** `demo_eval.py` imports `cv2` and `torchvision_x_functional` (which
   needs OpenCV and SciPy) at module level but uses them only on its XYZ path. They are replaced by
   stubs that raise on any attribute access, so a completed run proves the sRGB path did not use
   them. OpenCV and SciPy are not installed.
5. **Compiler and platform.** The extension is built with MSVC on Windows instead of GCC on Linux,
   from unmodified source; `Tensor::data<T>()` still exists in torch 2.14 (deprecated).
6. **`torch.load` safety.** torch 2.14 defaults to `weights_only=True`; both legacy-format (pre-1.6)
   checkpoints are plain tensor state dicts and load under it with strict key matching.
7. **CPU instead of CUDA.** The demo's own CPU branch is used. The checkpoints were saved from a
   CUDA run and were most likely trained through `models.py` (binsize `1.0001/32`, CUDA kernel
   compiled by nvcc, which contracts multiply-adds by default); that training-time arithmetic is
   not reproducible here and was not tested.
8. **JPEG decoding** by Pillow 12.3.0 / libjpeg-turbo 3.1.4.1. The demo input is a baseline 4:4:4
   JPEG (no chroma upsampling ambiguity), but the original decoder was not available for comparison.

No reference output from the authors is bundled in the repository, so this reproduction shows
that the documented path runs, is deterministic, and that independent implementations agree. It
cannot show equality with the authors' own historical output.

## Results

### Demo reproduction (`results/demo_reproduction.json`)

| | a1629.jpg (the demo input) | a1509.jpg (second bundled example) |
|---|---|---|
| Input file SHA-256 | `13b2259e0e84e7cc45e1cfd4376f80da2df646c2dcdd5f39e32804697235cf94` | `61734686655c83593d69189169d30368acf44926c17c84d8abddaa215a9f878e` |
| Input | 1620 x 1080 baseline JPEG, 4:4:4, ICC "sRGB IEC61966-2.1", Canon EOS-1D Mark II via Lightroom Classic 8.0; FiveK `a1629-JI2E5331` | 720 x 480, same encoding; FiveK `a1509-dvf_034` |
| Weights (no softmax) | `[1.9081297, -0.3463063, -0.9284921]` (sum 0.633) | `[1.6817590, -0.8801310, -0.8800941]` (sum -0.078) |
| Fused LUT | `f1a2574d8e0ca7254ee900eeb87eec67724ff4cbeada9e7bde8f812f5bc16d6e` | `efa7b30a6496c9c8607385ba9a82818f23f464313c565015269debb5e67a54f7` |
| Output float32 (1, 3, H, W) | `bc968771ba67fdb092e43a564a711793450f3c0f378be52ace0dc0c8a5fcff73` | `e63f3156ea89a2da32e392e340639e039c21e4781272b663bd4b16406bd4cf29` |
| Output uint8 (demo quantisation) | `a68b55139274abb6883bb5e7fa0256409d27226120d083ce128e432d6c142caf` | `82b613064d7430f4f0f327543834e138f113b1afba30113518146cd32fb4c919` |
| Extension = PyTorch = NumPy | bit-identical | bit-identical |

Checkpoints: `LUTs.pth` `c1bb2bc4b7239c1a7e96159f5923123ba796b1fceb0b8c3132b423ea825b821a`,
`classifier.pth` `bae9865395625ecae58cfe86147e521093bb1e29e7b2544e02adb238b8035021` (legacy
pickle format, CUDA storages, committed upstream on 2020-09-28). The JSON also records the decoded
input arrays, input tensors, per-tensor state-dict hashes, the demo patch diff, runtime warnings,
LUT diagnostics and the analyses below. Re-running gives identical values except timings.

Further analyses on the same inputs:

- **Determinism.** Repeated runs are bit-identical. PyTorch's classifier gives identical weights
  with 2 and 6 threads, but 1 thread moves them by up to 2.4e-7, which changes 6 of 5,248,800
  output samples by one code value. The recorded hashes therefore hold for this torch build, CPU
  capability (AVX2) and thread count (6); the regression test skips elsewhere.
- **Independent predictor.** The float64 NumPy classifier reproduces PyTorch's weights within
  3.4e-6; its fused LUT gives outputs within 2.4e-6 (409 samples differ by one code value).
- **Classifier input resolution.** The classifier resizes any input to 256 x 256 by bilinear
  interpolation without antialiasing, so the weights depend on the resolution supplied. For a1629,
  using a 720 x 480 copy (the 480p training scale) instead of 1620 x 1080 moves outputs by up to two
  code values; 360 x 240 by up to five.
- **What the demo LUT does.** Strong brightening (mean RGB 0.271/0.370/0.237 to 0.377/0.600/0.284 on
  a1629), black crush (2.3 % of a1629 samples and 2.9 % of a1509 samples fall below 0 and are clipped
  by the demo's quantisation) and, on a1629, neutral inputs above about 0.72 map above 1. 29 % of the
  a1629 LUT's entries exceed 1. Along the neutral ramp the interpolated output decreases slightly in
  places, only where it already exceeds 1.12, so no 8-bit output decreases.

Committed previews (rights-clean input): `results/<image>_lut_on_synthetic_chart.png` (a generated
test chart before and after each image's LUT) and `results/<image>_lut_neutral_curves.png` (fused LUT
nodes on the neutral axis). Photo previews (`build/previews/<image>_input_vs_output.png`, input and
output reduced 4x) are written locally but **not committed**: the photographs are MIT-Adobe FiveK
images under research-only licences that require the licence to accompany every copy.

### Trilinear parity and step 5 checks (`results/trilinear_parity.json`)

- **Exhaustive 8-bit parity.** All 16,777,216 8-bit colours (as `to_tensor` computes them) through the
  a1629 demo LUT, a seeded random LUT with values in [-0.5, 1.5] and the repository identity LUT:
  extension, PyTorch and NumPy agree **bit for bit** (0 of 150,994,944 samples differ).
- **Float edge cases.** 163 values (0, subnormals, 1 - ulp, 1, each cell boundary and its float32
  neighbours, cell midpoints, the census values below, the largest in-range input above 1):
  bit-identical across the three.
- **Full frame against tiles.** 256 x 256 tiles and single rows reproduce the full-frame output bit
  for bit (each pixel is independent).
- **Identity cube.** `IdentityLUT33.txt` is exactly the k/32 grid. With binsize `1.000001/32` it maps
  x to about x / 1.000001 (max error 1.2e-6; white becomes 0.99999905), and every 8-bit colour
  round-trips exactly through the demo's quantisation.
- **Corners and midpoints.** Outputs at the cube corners are within 4.6e-6 (demo LUT) and 1.2e-4
  (random LUT) of the corner nodes, because x = 1 lands at fraction 0.999968 of the top cell; cell
  midpoints are within 5.9e-6 and 9.0e-5 of the float64 ideal.
- **A discontinuity in the reference arithmetic.** The cell index is `floor(x / binsize)` (float32
  division) but the fraction is `fmod(x, binsize) / binsize` (exact remainder).
  - Checking every float32 in [0, 1] (1,065,353,217 values) finds **8** inputs (15 with the legacy
    binsize). Each is the largest float32 below a cell boundary (boundaries 17, 19, ..., 31 for
    `1.000001/32`); there the division rounds up to the boundary while the remainder does not.
  - The reference then uses the next cell with fraction 0.999999 and jumps one LUT node (about 1/32
    of the range for the identity cube).
  - **No 8-bit or 16-bit code value is affected**, so the demo is not. An unquantised float32
    pipeline does hit these inputs: 3 of 12.6 million seeded random float32 samples did, and the
    reference output there is off by up to 0.073 (demo LUT) or 1.94 (random LUT).
- **Two candidate fixes** (`fraction_variants` and the `exhaustive_8bit` entries):
  - *Quotient fraction* (`q - floor(q)`) is continuous but inherits float32 rounding of `q` (up to
    about 1e-6 near q = 31). It is within 6e-7 of the reference on the demo LUT but 4.3e-6 on the
    steep random LUT, and changes 303 of 50,331,648 demo-LUT 8-bit samples by one code value.
  - *Exact remainder* (index `floor(x / binsize)` as before, fraction `(x - i * binsize) / binsize`
    in double, clamped to [0, 1]) is **bit-identical to the reference on all 8-bit colours for all
    three LUTs**. On the random float32 pixels it differs only at the 3 pixels with census inputs,
    where it stays within 2.3e-6 of the float64 ideal (demo LUT). Reason: where the index is right,
    the double remainder is exactly upstream's `fmod` result, and for float32 operands a double
    division rounded to float32 equals the float32 division. ADR 0019 proposes it for the native
    kernel.
- **Domain.** The extension has no bounds checks: inputs below 0, above 1.0000008 (binsize
  `1.000001/32`) or NaN read outside the LUT. The NumPy and PyTorch versions refuse them.
- **Speed (indicative).** Extension 15.5-16.8 Mpx/s single-threaded, pure PyTorch about 7, NumPy
  about 2.7.

### Model summary (`results/model_summary.json`)

- **Classifier** (`models_x.Classifier`): bilinear resize to 256 x 256, then five 3 x 3 stride-2
  convolutions (3 to 16, 32, 64, 128, 128 channels), each followed by LeakyReLU(0.2), with
  InstanceNorm2d (affine, eps 1e-5, per-image statistics) after the first four, then Dropout(0.5)
  (identity in eval) and an 8 x 8 convolution to 3 outputs. 270,083 parameters (269,603 without
  the 480 InstanceNorm affine parameters, the paper's "269K"); 73,162,752 MACs (146.3 MFLOP;
  `torch.utils.flop_counter` agrees exactly), about 152.6 MFLOP with resize, normalisation and
  activations; fixed cost per image.
- **LUT basis.** 3 LUTs of 3 x 33^3 float32 (323,433 parameters, 1.29 MB), layout `[c][b][g][r]`
  (r fastest). Fusion: `w0*LUT0 + w1*LUT1 + w2*LUT2`. The model has 593,516 parameters in total
  (2.37 MB float32; "less than 600K" in the paper).
- **LUT application.** 76 floating-point operations and 24 LUT loads per pixel, 431 KB working set.
  Extension: 0.11 s for 1620 x 1080 single-threaded, so about 2.9 s for 45 MP on one core of the
  Ryzen 5 5500U (a projection, not a measurement).
- **Classifier time.** PyTorch: 4 ms (6 threads) to 22 ms (1 thread). The unoptimised float64 NumPy
  version: 0.1 s.

## Rights (summary; sources and quotes in ADR 0019)

- **Code**: Apache-2.0 (`LICENSE` added 2021-09-23, unfilled copyright line, no NOTICE file). Lucent
  may reimplement or port it with the licence's notice obligations. Not every file is Apache-2.0:
  `ssim.m` is "for educational and research purposes only".
- **Checkpoints**: no stated terms. They were committed a year before the licence and trained on
  MIT-Adobe FiveK. **Research only**: Lucent may not ship them, convert them, or ship anything
  derived from them.
- **MIT-Adobe FiveK** (including both bundled example photographs): Adobe and Adobe/MIT
  "RESEARCH LICENSE"s, "solely for your own research purposes" and not "directed toward commercial
  advantage or monetary compensation", with the licence included in every copy. **Research
  only**; not for training a shipped model.
- **Patents**: not established. A basic search found an adjacent patent (US 12,125,182 B2) but no
  freedom-to-operate review has been done.

## What was and was not shown

Shown: Appendix D2 steps 1-5 on CPU. The pinned snapshot was inspected; the checkpoint loads and
runs; inputs, checkpoints and outputs are hashed; resizing, channel and LUT-axis order,
interpolation boundaries and the unnormalised weight combination are preserved (no softmax); the
float32 trilinear application was validated on identity cubes, ramps, corners, midpoints and
tiles; and the upstream CPU extension was built and matched bit for bit.

Not shown, or not attempted:
- equality with the authors' historical outputs (none published);
- the training-time CUDA arithmetic and the PyTorch 0.4.1 `trilinear_c` build (only its `binsize`
  is emulated);
- FiveK test-set metrics;
- ONNX export and native C++ code (Appendix D2 steps 6 and 7 are later phases);
- a legal review or professional patent search.

Timings are single measurements on the research laptop.
