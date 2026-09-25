# Research reference environment

A pinned Python environment for Lucent developers (blueprint Appendix D1). It inspects camera
fixtures with rawpy/LibRaw, records reference values, and compares them with Lucent's native
probe. **It is never shipped, never needed to build or run Lucent, and nothing in the product
may import from it.**

## Setup

Requires CPython 3.12 x64 on Windows (the reference platform; `requirements.txt` carries
hashes for that platform's wheels). From the repository root, in PowerShell:

```powershell
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m venv research\.venv
research\.venv\Scripts\python.exe -m pip install -r research\requirements.txt
research\.venv\Scripts\python.exe -c "import rawpy; print(rawpy.__version__, rawpy.libraw_version)"
# 0.27.1 (0, 22, 1)
```

`research/.venv/` and `research/output/` are gitignored. Camera files come from
`tools\fetch-fixtures.ps1` (see `fixtures/README.md`).

## Tools (`research/tools/`)

| Tool | Purpose |
|---|---|
| `probe_reference.py` | Writes `lucent.probe-report` v1 reports with rawpy. `--decode` adds `raw_data_sha256`. With `--manifest` it probes the cached fixtures, checking each file's SHA-256 against the manifest first. |
| `compare_probe.py` | Compares the `parity` blocks of a Lucent report and a reference report, prints a table, exits non-zero on mismatch; supports a reviewed allowance file. |
| `raw_pixls_index.py` | Searches the raw.pixls.us archive index, drafts manifest entries, and verifies `fixtures/manifest.json` against the live index (licence, id, published hash). Standard library only. |

```powershell
$py = "research\.venv\Scripts\python.exe"
# Reference results for every cached fixture (full reports + committed compact references):
& $py research\tools\probe_reference.py --decode --manifest fixtures\manifest.json --tier all `
      --out-dir research\output\reference --parity-dir fixtures\reference
# One file to stdout:
& $py research\tools\probe_reference.py --decode fixtures\cache\dji-fc4382-dng\DJI_20250506184749_0011_D.DNG
# Compare Lucent's probe output with the reference:
& $py research\tools\compare_probe.py lucent-dji.json fixtures\reference\dji-fc4382-dng.parity.json
# Archive queries:
& $py research\tools\raw_pixls_index.py search "Sony" "ILCE-7M4"
& $py research\tools\raw_pixls_index.py verify-manifest
```

Exit codes: `probe_reference.py` 0 when every input was probed (a LibRaw refusal is a recorded
result, not a failure), 1 when an input is missing, fails its manifest hash or crashes the
probe, 2 for usage errors. `compare_probe.py` 0 match, 1 mismatch, 2 usage/schema error.

## Tests

```powershell
research\.venv\Scripts\python.exe -m unittest discover -s research\tests -v
```

- `test_compare_probe.py`: every comparison rule, allowances and schema rejection (synthetic).
- `test_probe_reference.py`: parity-block assembly on a stand-in object, raw-buffer hashing
  (endianness, non-contiguous views), error mapping, deterministic JSON; plus regression checks
  that the committed `fixtures/reference` files cover the manifest exactly and that re-probing the
  cached smoke fixtures reproduces them (skipped when fixtures are not downloaded).
- `test_fetch_fixtures.py`: runs `tools/fetch-fixtures.ps1` end to end with Windows PowerShell 5.1
  (and PowerShell 7 when installed) against a loopback HTTP server: verification before placement,
  cache reuse, `-Force`, corrupted cache, mirror fallback, tier/id selection, `-Bootstrap`, hash and
  size mismatches, HTTP errors and manifest/usage errors.
- `test_raw_pixls_index.py`: index parsing, mirror joining, file-name sanitising and manifest
  verification on synthetic data, plus structural checks of `fixtures/manifest.json`.

## Probe report contract (`lucent.probe-report`, schema version 1)

Shared by `research/tools/probe_reference.py` and Lucent's native `lucent-probe`. Readers reject
any other `schema_version`.

### Top level

| Field | Meaning |
|---|---|
| `schema`, `schema_version` | `"lucent.probe-report"`, `1`. |
| `file_name`, `file_size` | Input file name and size in bytes. |
| `file_sha256` | Lowercase SHA-256 of the input (optional for other generators; compared when both sides have it). |
| `generator` | Producer identity. rawpy: `{"name": "rawpy-reference", "rawpy": rawpy.__version__, "libraw": rawpy.libraw_version, "flags": rawpy.flags, "numpy": ..., "python": ...}`. |
| `status`, `error` | `"ok"`, or `"error"` with `error = {stage, type, libraw_code, message}` (rawpy reference; other generators may use their own error shape). |
| `parity` | The block below, or `null` when the decoder refused the file. |
| other fields | Generator specific (for example `fixture_id`, `options`, and rawpy-only diagnostics under `reference`). Not compared. |

### Parity block

All values are read **after a successful `LibRaw::unpack()`** of shot 0 (`shot_select = 0`) and
without any post-processing. Each field has exactly the semantics of the rawpy 0.27.1 property of
the same name (`rawpy/_rawpy.pyx`, `rawpy/data_helper.h` at tag `v0.27.1`):

| Field | Type | Exact source |
|---|---|---|
| `raw_width`, `raw_height`, `width`, `height`, `top_margin`, `left_margin`, `iwidth`, `iheight`, `flip` | int | `imgdata.sizes.*` after unpack (rawpy's `sizes` property does not unpack by itself; the parity values are defined post-unpack). |
| `num_colors` | int | `imgdata.idata.colors`. |
| `color_desc` | string | `imgdata.idata.cdesc` (ASCII, up to the NUL). Monochrome files still report `"GMCY"` or similar; `num_colors` says how many letters matter. |
| `raw_pattern` | int[n][n] or null | `null` when `imgdata.rawdata.raw_image == NULL`. Otherwise n = 1 if `idata.filters == 0`, 16 if `filters == 1` (Leaf), 6 if `filters == 9` (X-Trans), 4 if `filters >= 1000` (other values below 1000: rawpy raises `NotImplementedError`). Entry `[y][x] = LibRaw::COLOR(y - top_margin, x - left_margin)`; for n = 4 the matrix is reduced to its top-left 2x2 when all four 2x2 quadrants are equal. Monochrome gives `[[6]]` because `COLOR()` returns 6 when `filters == 0`. Call LibRaw's own `COLOR()` rather than re-implementing it. |
| `black_level_per_channel` | int[4] | rawpy `adjust_bl_()` applied to a copy of `imgdata.rawdata.color`: fold the `cblack[4] x cblack[5]` pattern at `cblack[6..]` into `cblack[0..3]` (for `filters > 1000` when both pattern dimensions are 1 or 2, using `FC()` to map the cells of the top-left 2x2 to channels, the last green mapped to channel 3 when there are two; for `filters <= 1000` only a 1x1 pattern), move the minimum of `cblack[0..3]` into `black`, move the minimum of the remaining pattern into `black`, then add `black` to each of `cblack[0..3]`. It reproduces `LibRaw::adjust_bl()`; mirror that code exactly, including its unsigned arithmetic. |
| `white_level` | int | `imgdata.rawdata.color.maximum`. |
| `camera_white_level_per_channel` | int[4] or null | `imgdata.rawdata.color.linear_max[0..3]` when all four are > 0, else `null`. |
| `camera_whitebalance` | float[4] | `imgdata.rawdata.color.cam_mul[0..3]`, not normalised (Fujifilm reports values such as `[578, 302, 661, 0]`). |
| `daylight_whitebalance` | float[4] | `imgdata.rawdata.color.pre_mul[0..3]`. |
| `rgb_xyz_matrix` | float[4][3] | `imgdata.rawdata.color.cam_xyz`. |
| `color_matrix` | float[3][4] | `imgdata.rawdata.color.cmatrix`, **not** `rgb_cam`. rawpy's `color_matrix` reads `cmatrix` (the camera matrix read from the file); it is all zeros for most non-DNG fixtures, while `rgb_cam` is not. |
| `raw_type` | string | `"flat"` when `imgdata.rawdata.raw_image != NULL`, else `"stack"` (`color3_image` / `color4_image`). |
| `raw_data_sha256` | string | Only when decoding and only for `"flat"`: lowercase SHA-256 of `raw_image` as `raw_height` rows of `raw_width` uint16 samples, little-endian, row-major, no padding. Hash the unpacked data before any DNG opcode, black subtraction or scaling. rawpy assumes rows are `raw_width` samples apart; a native implementation should step rows by `sizes.raw_pitch` (bytes) and hash `raw_width` samples per row. |

Note the colour fields come from `imgdata.rawdata.color`, LibRaw's snapshot taken during
`unpack()`, not from `imgdata.color`, which later processing may change.

Numbers: floats are LibRaw's `float` values widened exactly to double; non-finite values are
written as the strings `"nan"`, `"inf"`, `"-inf"`. `compare_probe.py` compares integers, strings
and patterns exactly and floats with `|a - b| <= max(1e-6 * max(|a|, |b|), 1e-9)`.

Errors: rawpy raises one exception class per LibRaw error code; the reference records the class
name and the code (`LIBRAW_FILE_UNSUPPORTED` = -2, `LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE` = -3,
`LIBRAW_UNSUFFICIENT_MEMORY` = -100007, `LIBRAW_DATA_ERROR` = -100008, `LIBRAW_IO_ERROR` = -100009,
...). rawpy also raises `LibRawDataError` when `LibRaw::error_count() > 0` after an otherwise
successful call, so a native adapter that tolerates LibRaw's non-fatal data warnings will differ
from the reference on damaged files.

### Allowances

Differences that are understood and reviewed are recorded in an allowance file, never by editing
reference files:

```json
{
  "schema": "lucent.parity-allowances",
  "schema_version": 1,
  "allowances": [
    {
      "field": "daylight_whitebalance",
      "files": ["*.ORF"],
      "action": "tolerance",
      "rel_tol": 1e-4,
      "abs_tol": 1e-9,
      "reason": "What differs, why it is correct (e.g. the LibRaw commit), and who reviewed it."
    }
  ]
}
```

`action` is `"tolerance"` (float fields only; widens the float tolerance) or `"ignore"` (the
difference is reported as allowed). `files` holds optional case-insensitive `fnmatch` patterns on
`file_name`. A non-empty `reason` is mandatory; unknown keys and actions are rejected. Allowances
that were not needed are listed in the output so stale entries are noticed. No allowance file is
committed yet: none has been justified.

## Decoder builds: rawpy's LibRaw versus Lucent's LibRaw

| | rawpy 0.27.1 wheel (reference) | vcpkg `libraw` 0.22.2 (Lucent) |
|---|---|---|
| LibRaw version | 0.22.1 (submodule `b860248a89d9082b8e0a1e202e516f46af9adb29`) | 0.22.2 (2026-07-16) |
| OpenMP | on | off |
| LCMS | on | on |
| DNG deflate (zlib) / DNG lossy (libjpeg) | on / on | on / on |
| RedCine codec | on | off (not compiled) |
| Foveon X3F tools | on | off |
| Raspberry Pi 6BY9 | on | off |
| RawSpeed, GPL demosaic packs, Adobe DNG SDK | off | off |

Blueprint D1 asks for the native adapter to be compared with the reference **on the same decoder
build**. That is not the case today: the reference is LibRaw 0.22.1 with a different feature set.
What this means:

- **Version.** 0.22.2 is a bug-fix and hardening release (changelog in the vcpkg source tree):
  allocation and overflow checks (CR3/CRX, Fuji, float DNG, X3F), recursion limits in the QuickTime
  and MOS parsers, zeroed read buffers, `open_bayer()` input checks, a refusal of Olympus 14-bit
  files with incorrect `wbits` metadata, and whitespace fixes in make/model strings. None of these
  is expected to change the parity block of a well-formed fixture, but a hardening check can turn
  a borderline file into a refusal (the Olympus `wbits` rule is the one to watch for ORF files), and
  make/model text may differ, which is why make/model is not in the parity block.
- **Build flags.** The X3F, Raspberry Pi and RedCine differences affect only Sigma X3F, Raspberry Pi
  and RED files, none of which is in the corpus. OpenMP parallelises decoding and processing
  but must not change decoded values; `raw_data_sha256` checks exactly that.
- **Consequence.** A mismatch is a finding to investigate, not automatically a Lucent bug. If it
  traces to a 0.22.1 to 0.22.2 change, record it as an allowance citing the LibRaw commit.
  Agreement on all fixtures is strong evidence but not the same-build proof D1 asks for.
- **Closing the gap** (not done in W1): build rawpy from source against the vcpkg LibRaw 0.22.2
  with matching flags (needs Cython and an MSVC build of rawpy), or wait for a rawpy release that
  bundles 0.22.2 and move the pin. Either way, regenerate `fixtures/reference` and record the new
  `generator` block.

## Observations from the reference run (2026-09-24)

34 fixtures probed; 32 decoded, 2 refused (Nikon HE and HE*: `LIBRAW_FILE_UNSUPPORTED` at
unpack, consistent with LibRaw 0.22 supporting only standard NEF compression on the Z 8).

- **Routes.** Bayer RGGB and GRBG (DJI, Insta360), X-Trans 6x6, monochrome (`[[6]]`), and "stack"
  data for Canon sRAW, Sony lossless M/S (YCC pseudo-RAW that LibRaw marks as already white
  balanced) and the Adobe LinearRaw DNG.
- **Colour data provenance differs by maker.** `rgb_xyz_matrix` (LibRaw's built-in table) is set
  and `color_matrix` is zero for Canon, Sony, Nikon, Fujifilm and Panasonic; DJI, Hasselblad
  L2D-20c and Insta360 DNGs have a zero `rgb_xyz_matrix` and only a file matrix in `color_matrix`;
  some files have both; the Leica M Monochrom has neither. The engine must track which source
  it uses (blueprint section 3: colour data "with provenance").
- **DNG 1.7 with an enhanced image** (`canon-eos-7d-mark-ii-dng-jxl`): LibRaw decodes only the
  CFA original; the JPEG XL "Enhanced" LinearRaw image is neither decoded nor counted as a shot.
- **Dual Pixel RAW** (`canon-eos-r5-cr3-craw-crop-dualpixel`): LibRaw exposes two shots
  (`shot_select` 0 and 1); ordinary CR3 files expose one.
- **Determinism.** Two consecutive runs produced byte-identical full reports and parity files.

Full reports (with rawpy-only diagnostics such as lens, exposure and visible-area min/max) are in
`research/output/reference/<id>.json` after a run.
