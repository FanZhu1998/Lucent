# Camera test fixtures

Real camera RAW files used by Lucent's fixture-dependent tests and by the decoder-parity work of
blueprint Appendix D1. They support the camera-qualification families of blueprint section 3 and
the qualification corpus of section 10. They are **planned test inputs, not a claim that Lucent
supports these cameras**; support states come from Lucent's own qualification results.

The RAW files are **not committed**. `manifest.json` lists them, `tools/fetch-fixtures.ps1`
downloads and verifies them into `fixtures/cache/` (gitignored), and `reference/` holds small
committed rawpy/LibRaw reference results for each file.

## Licence

Every fixture comes from [raw.pixls.us](https://raw.pixls.us/) and is individually marked
**CC0 1.0 Universal** ([public-domain dedication](https://creativecommons.org/publicdomain/zero/1.0/))
in the archive's per-file licence column. The machine-readable index is
<https://raw.pixls.us/json/getrepository.php?set=all> (rendered at <https://raw.pixls.us/#repo>).
Uploaders declare on the upload form at <https://raw.pixls.us/> that they own full rights to the
file and release it under CC0.

The archive is **not uniformly CC0**: on 2026-09-24, 146 of its 2016 files were
CC BY-NC-SA 4.0 imports from rawsamples.ch. None of those is used. The manifest accepts only
`"license": "CC0-1.0"`, the downloader refuses any other value, and
`research/tools/raw_pixls_index.py verify-manifest` re-checks every entry against the live index
(licence, sample id, make/model and the archive's published SHA-256). The licence was checked on
2026-09-24.

## Layout

```
fixtures/
  manifest.json                 lucent.fixture-manifest v1 (committed)
  reference/<id>.parity.json    rawpy reference probe results (committed, ~4 KB each)
  cache/<id>/<file_name>        downloaded RAW files (gitignored)
```

Tests receive the directory as the `LUCENT_FIXTURE_DIR` compile definition
(`cmake/LucentTargets.cmake`), so a fixture's path is
`LUCENT_FIXTURE_DIR/cache/<id>/<file_name>` and its reference is
`LUCENT_FIXTURE_DIR/reference/<id>.parity.json`. A test must call `GTEST_SKIP()` with a clear
message when the cached file is absent, so a fresh checkout still passes
(`docs/development/conventions.md`).

## Fetching

From the repository root, in Windows PowerShell 5.1 or PowerShell 7:

```powershell
powershell -NoProfile -File tools\fetch-fixtures.ps1                      # smoke tier (default), ~154 MB
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Tier qualification  # ~998 MB
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Tier all            # everything, ~1.15 GB
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Id dji-fc4382-dng,nikon-z-6-nef-lossless
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Id canon-eos-r5-cr3-raw -Force
```

- Each file is downloaded to a temporary `.partial` file beside its destination, and its size and
  SHA-256 are verified **before** it is moved into place; a bad download never replaces a file.
- Cached files are re-hashed on every run: verified files are skipped, files that fail
  verification are downloaded again. `-Force` downloads again regardless.
- `url` (the archive's id-based `getfile.php` link) is tried first, then `mirror_url` (the static
  mirror under `https://raw.pixls.us/data/`); a fallback prints a warning naming the failed URL.
- The archive answers an unknown sample id with HTTP 200 and an empty body, so the HTTP status
  proves nothing: verification does. Empty files and HTML/XML payloads are always rejected, even
  with `-Bootstrap`.
- Exit status: `0` all selected fixtures verified, `1` any fixture failed (the others are still
  processed), `2` usage or manifest error.
- `-Bootstrap` is only for adding fixtures: an entry without `sha256` is otherwise refused. With
  `-Bootstrap` the file is fetched and `BOOTSTRAP id=<id> sha256=<hex> size_bytes=<n>` is printed
  for recording in the manifest. Bootstrapped files are unverified until the hash is recorded.

## Manifest format (`lucent.fixture-manifest`, version 1)

Top level: `schema`, `schema_version`, `license_note`, `sources` (where and when the archive was
checked), `tiers` (descriptions), `gaps` (targets that are absent or deferred) and `fixtures`.
Readers reject a newer `schema_version`; unknown extra fields are ignored.

| Field | Meaning |
|---|---|
| `id` | Stable identifier, `^[a-z0-9]+(-[a-z0-9]+)*$`; also the cache and reference file name. Never reuse an id for different bytes. |
| `tier` | `smoke` or `qualification`. |
| `make`, `model` | As listed by the archive (for drones this is the camera module, e.g. `FC4382`). |
| `format`, `mode` | Container (file extension) and capture mode/compression. |
| `url`, `mirror_url` | Download locations, tried in that order. |
| `file_name` | Camera's original file name, restricted to `[A-Za-z0-9._()+-]`. |
| `size_bytes`, `sha256` | Exact size and lowercase SHA-256 computed after download. Both matched the archive's published checksums. |
| `license` | SPDX identifier; always `CC0-1.0`. |
| `source_page` | Mirror directory listing the file. |
| `raw_pixls_id` | The archive's sample id (cross-checked by `verify-manifest`). |
| `notes` | What the file exercises, verified against the file's own structure. |

## Tiers

- **smoke** (5 files, 154 MB): one file per major decoder route (Canon CR3, Sony ARW, a DJI DNG
  whose OpcodeList3 carries a lens correction, Fujifilm X-Trans RAF, Nikon NEF). This is the
  default download and the tier that fixture-dependent tests should prefer.
- **qualification** (29 files, 998 MB): the wider camera and mode matrix of blueprint section 3.

## Coverage

MB = 10^6 bytes. The last column is what rawpy 0.27.1 (bundled LibRaw 0.22.1) reports after
`unpack()`; the exact values are in `reference/<id>.parity.json`. "Stack" means LibRaw returns 3
components per pixel instead of a mosaic.

| Id | Tier | Camera | Format / mode | MB | LibRaw 0.22.1 result (rawpy) |
|---|---|---|---|---:|---|
| `canon-eos-r5-cr3-raw` | smoke | Canon EOS R5 | CR3: RAW, full frame, no Dual Pixel | 40.6 | flat, Bayer RGGB, 8191x5463 |
| `sony-ilce-7m4-arw-compressed` | smoke | Sony ILCE-7M4 | ARW: Compressed RAW (lossy), full frame | 40.7 | flat, Bayer RGGB, 7028x4688 |
| `dji-fc4382-dng` | smoke | DJI FC4382 (Mavic 3 Pro, 70 mm) | DNG: lossless JPEG tiles; OpcodeList3 WarpRectilinear | 14.9 | flat, Bayer GRBG, 4032x3024 |
| `fujifilm-x-t4-raf-lossless` | smoke | Fujifilm X-T4 | RAF: 14-bit lossless compressed | 27.9 | flat, X-Trans 6x6, 6246x4170 |
| `nikon-z-6-nef-lossless` | smoke | Nikon Z 6 | NEF: 14-bit lossless compressed | 30.2 | flat, Bayer RGGB, 6064x4040 |
| `canon-eos-r5-cr3-craw` | qualification | Canon EOS R5 | CR3: C-RAW, full frame | 16.8 | flat, Bayer RGGB, 8191x5463 |
| `canon-eos-r5-cr3-raw-crop` | qualification | Canon EOS R5 | CR3: RAW, 1.6x crop | 17.4 | flat, Bayer RGGB, 5087x3391 |
| `canon-eos-r5-cr3-craw-crop-dualpixel` | qualification | Canon EOS R5 | CR3: C-RAW, 1.6x crop, Dual Pixel RAW (2 images) | 11.4 | flat, Bayer RGGB, 5087x3391 |
| `canon-eos-5d-mark-iv-cr2-raw` | qualification | Canon EOS 5D Mark IV | CR2: RAW | 33.9 | flat, Bayer RGGB, 6744x4502 |
| `canon-eos-5d-mark-iv-cr2-sraw` | qualification | Canon EOS 5D Mark IV | CR2: sRAW (YCbCr) | 15.2 | stack, 3360x2240 |
| `sony-ilce-7m4-arw-uncompressed` | qualification | Sony ILCE-7M4 | ARW: uncompressed | 73.5 | flat, Bayer RGGB, 7028x4688 |
| `sony-ilce-7m4-arw-lossless-l` | qualification | Sony ILCE-7M4 | ARW: lossless compressed, size L | 49.2 | flat, Bayer RGGB, 7028x4688 |
| `sony-ilce-7m4-arw-lossless-m` | qualification | Sony ILCE-7M4 | ARW: lossless compressed, size M (YCC) | 32.8 | stack, 4624x3080 |
| `sony-ilce-7m4-arw-lossless-s` | qualification | Sony ILCE-7M4 | ARW: lossless compressed, size S (YCC) | 22.9 | stack, 3516x2344 |
| `sony-ilce-7m4-arw-compressed-apsc` | qualification | Sony ILCE-7M4 | ARW: compressed (lossy), APS-C crop | 19.6 | flat, Bayer RGGB, 4692x3132 |
| `sony-ilce-7rm5-arw-compressed` | qualification | Sony ILCE-7RM5 | ARW: compressed (lossy) | 72.3 | flat, Bayer RGGB, 9564x6376 |
| `dji-fc220-dng` | qualification | DJI FC220 (Mavic Pro) | DNG: uncompressed; OpcodeList1 FixBadPixelsList, OpcodeList3 FixVignetteRadial | 24.3 | flat, Bayer RGGB, 4000x3000 |
| `hasselblad-l2d-20c-dng` | qualification | Hasselblad L2D-20c (DJI Mavic 3) | DNG: uncompressed; OpcodeList3 GainMap, WarpRectilinear | 43.6 | flat, Bayer RGGB, 5280x3956 |
| `dji-fc7303-dng` | qualification | DJI FC7303 (Mini 2) | DNG: uncompressed; OpcodeList3 FixVignetteRadial, WarpRectilinear, GainMap | 24.5 | flat, Bayer RGGB, 4000x3000 |
| `insta360-one-x-dng` | qualification | Arashi Vision Insta360 One X | DNG: unstitched dual-fisheye 360 source | 37.0 | flat, Bayer GRBG, 3040x6080 |
| `nikon-z-8-nef-he` | qualification | Nikon Z 8 | NEF: High Efficiency (HE) | 24.4 | refused at unpack: `LibRawFileUnsupportedError` (-2) |
| `nikon-z-8-nef-he-star` | qualification | Nikon Z 8 | NEF: High Efficiency* (HE*) | 35.7 | refused at unpack: `LibRawFileUnsupportedError` (-2) |
| `nikon-z-6-nef-compressed` | qualification | Nikon Z 6 | NEF: 14-bit compressed (lossy) | 26.0 | flat, Bayer RGGB, 6064x4040 |
| `fujifilm-x-t4-raf-compressed` | qualification | Fujifilm X-T4 | RAF: 14-bit compressed (lossy) | 27.0 | flat, X-Trans 6x6, 6246x4170 |
| `fujifilm-gfx-50s-ii-raf-compressed` | qualification | Fujifilm GFX50S II | RAF: Bayer (medium format), compressed | 34.5 | flat, Bayer RGGB, 8280x6208 |
| `panasonic-dc-s5m2-rw2` | qualification | Panasonic DC-S5M2 | RW2: 14-bit | 37.3 | flat, Bayer RGGB, 6008x4008 |
| `om-system-om-1-orf` | qualification | OM System OM-1 | ORF: standard 20 MP | 21.8 | flat, Bayer RGGB, 5220x3912 |
| `pentax-k-3-mark-iii-pef` | qualification | Pentax K-3 Mark III | PEF: 14-bit | 32.3 | flat, Bayer RGGB, 6224x4160 |
| `pentax-k-3-mark-iii-monochrome-dng` | qualification | Pentax K-3 Mark III Monochrome | DNG: LinearRaw, 1 sample per pixel | 37.7 | flat, monochrome, 6224x4160 |
| `leica-m10-dng` | qualification | Leica M10 | DNG: CFA, lossless JPEG | 34.2 | flat, Bayer RGGB, 5984x3992 |
| `leica-m-monochrom-typ-246-dng` | qualification | Leica M Monochrom (Typ 246) | DNG: big-endian LinearRaw, 1 sample per pixel | 21.7 | flat, monochrome, 5984x4000 |
| `hasselblad-x1d-ii-50c-3fr` | qualification | Hasselblad X1D II 50C | 3FR | 108.4 | flat, Bayer RGGB, 8278x6208 |
| `adobe-dng-canon-eos-5d-mark-iii-linear-dng` | qualification | Canon EOS 5D Mark III via Adobe DNG Converter | DNG: LinearRaw (demosaiced), 3 samples per pixel | 24.1 | stack, 3960x2640 |
| `canon-eos-7d-mark-ii-dng-jxl` | qualification | Canon EOS 7D Mark II via Lightroom Denoise | DNG 1.7: CFA original + JPEG XL "Enhanced" LinearRaw image | 38.3 | flat, Bayer RGGB, 5496x3670 (the CFA original only) |

Route coverage: Bayer mosaics in RGGB and GRBG order, X-Trans, monochrome sensors, 3-component
"stack" data (Canon sRAW, Sony YCC lossless M/S, LinearRaw DNG), DNG opcode lists
(WarpRectilinear, FixVignetteRadial, GainMap, FixBadPixelsList), crop modes, multi-image files
(Dual Pixel RAW, DNG 1.7 with an enhanced image), a big-endian DNG, and two files LibRaw 0.22
refuses (Nikon HE/HE*), which exercise the Unsupported path.

Opcode lists and IFD layouts were read from the files themselves, not from archive metadata.

## Gaps (recorded in `manifest.json` under `gaps`)

| Target | Status |
|---|---|
| DJI Mini 4 Pro (FC8482) | **Absent** from the archive. `dji-fc7303-dng` (Mini 2) is the closest Mini-series camera. |
| Insta360 Ace / Ace Pro / Ace Pro 2 planar DNG | **Absent**. The only Insta360 (Arashi Vision) sample is the One X dual-lens 360 source. |
| Fujifilm multi-shot / pixel-shift RAF | **Absent**. |
| Pixel-shift and high-resolution composites (Sony ARQ, Pentax, Panasonic, OM System) | Available (40-325 MB each) but **deferred** until the multi-image route exists. |
| Corrupted and truncated files | **Derived** at test time from cached fixtures, not downloaded. |
| Scene diversity (sunset, blue hour, night sky, fog, snow, high ISO, backlight) | **Absent**: the archive holds mostly one daylight test shot per camera mode. Blueprint section 10's scene corpus needs a separate, separately licensed collection. |

Further CC0 modes exist in the archive but were not selected to keep the download modest, for
example Canon EOS R5 full-frame Dual Pixel RAW (sample 4693), Sony A7 IV APS-C lossless M/S
(6934, 6935) and Nikon Z 8 14-bit lossless (6617). Add them with the procedure below when a test
needs them.

## Reference results (`reference/<id>.parity.json`)

Each file is a `lucent.probe-report` v1 document produced by
`research/tools/probe_reference.py --decode` with the pinned rawpy environment: file identity
(`file_name`, `file_size`, `file_sha256`), the `generator` block (rawpy and LibRaw versions and
build flags), `status`/`error`, and the `parity` block that Lucent's LibRaw adapter must
reproduce. The contract, and why rawpy's bundled LibRaw (0.22.1) differs from Lucent's vcpkg build
(0.22.2), are documented in `research/README.md`. To regenerate after changing the manifest or
the rawpy pin:

```powershell
research\.venv\Scripts\python.exe research\tools\probe_reference.py --decode --manifest fixtures\manifest.json --tier all --out-dir research\output\reference --parity-dir fixtures\reference
```

## Adding a fixture

1. Find the sample: `research\.venv\Scripts\python.exe research\tools\raw_pixls_index.py search "Nikon" "Z 8"`.
   Use only rows whose licence column says `CC0-1.0`.
2. Draft the entry: `... raw_pixls_index.py describe <sample id>`; paste it into `fixtures`, then
   set `id`, `tier`, `mode` and `notes` (verify claims about the file's structure).
3. `powershell -NoProfile -File tools\fetch-fixtures.ps1 -Id <id> -Bootstrap`, then record the
   printed `sha256` and `size_bytes`.
4. `... raw_pixls_index.py verify-manifest` (the hash must equal the archive's published one), and
   `tools\fetch-fixtures.ps1 -Id <id>` must now report `cached, verified`.
5. Regenerate the reference files (command above) and run the research tests
   (`research\README.md`). Commit `manifest.json` and the new `reference/<id>.parity.json`.

Keep the smoke tier small (4-6 files, under 250 MB); add breadth to the qualification tier.
