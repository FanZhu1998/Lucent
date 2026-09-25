# ADR 0005: RAW decoder boundary and decoded-RAW data contract

- **Status:** Accepted
- **Date:** 2026-09-25 (revised the same day after the W1 review: contract 1.1)
- **Workflow:** W1 (Foundation and RAW probe)

## Context

Blueprint section 3 ("Adapter boundary") asks for a versioned `RawDecoder` interface with
`probe`, `decode`, cancellation, limits and structured error reporting. Its result must carry
source identity and subimage, format and compression, camera and lens identity, dimensions,
active area and orientation; a `MosaicFrame`, `LinearColorImage` or `MonochromeFrame` with its
sample encoding, CFA layout, channel mapping and any processing already applied; black and white
levels, as-shot neutral, colour data, illuminants and noise metadata with provenance; and
required gain maps or opcodes, warnings and the decoder capabilities used. The first image of a
multi-image file must never be chosen silently, and distinct sensor channels (both greens) must
be preserved. Section 5, step 1 requires validating the representation before choosing an
adapter and preserving the original data. Section 7 requires stable-read checks around
decoding. Appendix D1 asks that the native adapter be compared with a rawpy reference.

Facts verified for this decision (vcpkg LibRaw 0.22.2, headers and sources in the build tree):

- The `libraw::raw_r` target is the thread-safe build (`raw` is built with `LIBRAW_NOTHREADS`).
  OpenMP is off, the Adobe DNG SDK is absent (`LibRaw::capabilities()` reports `zlib` and
  `jpeg` only), so LibRaw never applies DNG opcode lists.
- `libraw.h` enables `LIBRAW_WIN32_UNICODEPATHS` for MSVC, so `open_file(const wchar_t*)` exists.
  It opens the file with `CreateFileW(..., FILE_SHARE_READ, ...)` through
  `LibRaw_bigfile_buffered_datastream` (src/libraw_datastream.cpp), without a `\\?\` prefix, and
  reports every open failure as `LIBRAW_IO_ERROR`. (`LibRaw::capabilities()` reports
  `unicodepaths` only in builds with the DNG SDK, src/utils/utils_libraw.cpp, so its absence from
  Lucent's capability list says nothing about the overload.)
- LibRaw maps both its end-of-data and "corrupt structure" exceptions to `LIBRAW_IO_ERROR`
  (internal/libraw_cxx_defs.h, `EXCEPTION_HANDLER`), and several decoders report success when the
  file ends inside the pixel data (tiled DNG, Sony ARW, Canon sRAW, OM System ORF; measured on
  truncated copies of the fixtures).
- LibRaw converts floating-point DNG data to rescaled integers unless
  `LIBRAW_RAWOPTIONS_CONVERTFLOAT_TO_INT` is cleared (it is set by default).
- LibRaw does not report the compression mode as such. It parses the camera's own record of it
  into maker-note fields (`canon.Quality` 4 = RAW, 7 = C-RAW; `nikon.NEFCompression`;
  `sony.SonyRawFileType`), and keeps the Fujifilm compressed-data header's lossless flag and the
  Panasonic RW2 encoding only in its unpacker state (`libraw_internal.h`, reachable through the
  public `get_internal_data_pointer()`). Measured on the fixtures: R5 RAW Quality 4, C-RAW 7;
  Z 6 lossy NEFCompression 4, lossless 3; A7 IV lossy SonyRawFileType 2, lossless 4,
  uncompressed 0; X-T4 compressed flag 0, lossless 1.
- LibRaw counts a Canon Dual Pixel RAW as two images (two full-size CRX tracks; LibRaw's own
  source calls the first "Canon dual pixel, 1st frame") and a CR3 RAW burst as the frames of one
  track. Decoding both images of the R5 Dual Pixel fixture gives a mean signal above black of
  476 and 256: the second image is the single-photodiode frame.
- LibRaw folds DNG BlackLevelDeltaH/V into its common black level as their rounded mean.
- For DNG, LibRaw moves an odd ActiveArea margin to the next even row or column; the CFA stays
  anchored to the ActiveArea origin, as DNG specifies.
- rawpy 0.27.1 (the reference) reads colour data from `imgdata.rawdata.color` after `unpack()`,
  and its `color_matrix` is LibRaw's `cmatrix`, not `rgb_cam`.

## Decision

1. **Interface.** `lucent::raw::RawDecoder` (`raw_decoder.hpp`) has `id()`, `probe(path,
   ProbeOptions, CancellationToken)` and `decode(path, DecodeOptions, CancellationToken)`, both
   returning `lucent::Expected`. Implementations are immutable and their const members may be
   called concurrently. No exception crosses the boundary. Errors use the categories listed in
   `raw_decoder.hpp`, with `where` = `raw.<adapter>.<stage>` and the library code in
   `native_code`.
2. **Versioned, decoder-agnostic contract.** `decoded_raw.hpp` defines `RawMetadata`,
   `ProbeResult`, `DecodedRaw` and the frame types, versioned by `kDecodedRawContractVersion`
   (1.1). No third-party type appears in a public header. All enums have persisted numeric values
   and only grow. Adding a field is a minor change; changing the meaning of a field or of pixel
   values is a major change, rejected by readers of the old major version
   (`core::check_readable`). Version 1.1 (after the W1 review) added `DataCompression::provenance`
   and `basis`, `SubimageRole`, `ProjectionInfo`/`ViewLayout`, `NoiseProfile::source_ifd`,
   `Provenance::LucentRule` and interpretation issue kinds 9-13.
3. **Pixels are copied and never altered.** The adapter copies LibRaw's buffer into a
   Lucent-owned `image::ImageBuffer`, stepping rows by LibRaw's `raw_pitch` in bytes, and then
   releases the LibRaw instance. A `MosaicFrame` or `MonochromeFrame` holds the full raw frame
   (`raw_width` x `raw_height`, masked margins included) with one sample per site; a
   `LinearColorImage` holds three channels (LibRaw's fourth, unused slot is dropped). Samples are
   exactly what the decoder produced: no black subtraction, no scaling, no clipping and no white
   balance. Floating-point DNG data stays `float` (`CONVERTFLOAT_TO_INT` is cleared).
4. **Levels and colour data are reported, not applied.** `SensorLevels` gives the common,
   per-channel and spatial black levels (the spatial pattern is anchored at the visible-area
   origin, as LibRaw applies it), the white level and per-channel linear limits, with a
   `Provenance` and the `MetadataStage` at which they were read. A probe reports values after
   opening; a decode reports values after unpacking (LibRaw measures some black levels while
   unpacking). Colour data is LibRaw's `cam_mul`, `pre_mul`, `cam_xyz`, `cmatrix` and `rgb_cam`;
   DNG camera-profile tags, NoiseProfile and opcode lists come from Lucent's own TIFF/DNG
   inspector (`tiff_inspector.hpp`), which is authoritative for DNG structure. NoiseProfile is
   taken from the decoded raw IFD, else from IFD 0 (DNG 1.7.1.0 places it in the raw IFD, but
   the DJI and Hasselblad drone DNGs of the corpus store it only in IFD 0 and Adobe's converter
   writes both); `NoiseProfile::source_ifd` records which. Both copies are verified against the
   colour planes. Floating-point DNG data without WhiteLevel uses the DNG default 1.0 (with a
   decoder warning). `SensorLevels` has no per-row or per-column term, so a DNG with non-zero
   BlackLevelDeltaH/V gets an `UnrepresentedBlackLevelCorrection` issue (the values stay
   available in `DngMetadata::raw_ifd`, as do MaskedAreas).
5. **Channel mapping and CFA.** Channel indices are LibRaw's colour indices; `channel_colors`
   names them (`R, G, B, G2` for LibRaw's Bayer data, where index 3 is the green sharing rows with
   blue). The CFA is an `image::CfaPattern` relative to the raw-frame origin, computed from
   LibRaw's own `COLOR()`; `cfa_at_visible_origin()` gives the visible-area frame.
6. **Subimages are explicit.** Metadata states `subimage_index` and `subimage_count` (LibRaw
   `raw_count`). A probe describes every subimage (up to 16) without unpacking. A decode of a
   file with more than one image fails with `InvalidArgument` unless `DecodeOptions::subimage` is
   set; an index beyond the count is `NotFound`. LibRaw counts Canon Dual Pixel frames and every
   DNG IFD with NewSubFileType 0 as images; for a DNG with several raw IFDs the inspector cannot
   tell which IFD a subimage is, so such metadata is `Ambiguous` and carries no DNG tags.
   Every image has a `SubimageRole`: `Primary` (single-image files, image 0 of a Dual Pixel
   RAW), `DualPixelComponent` (image 1 of a Canon Dual Pixel RAW, identified from the CR3 track
   layout as LibRaw selects tracks; issue `SubimageNotAPhotograph`, so never verified),
   `BurstFrame` (a CR3 RAW burst frame: a complete exposure), `PixelShiftFrame` (Pentax maker
   note Quality 7/8; issue `SubimageFramesNotCombined`) or `Unknown` (anything else with several
   images; issue `SubimageRoleUnknown`, `Ambiguous`). The probe's subimage list carries each
   image's role and interpretation status, so a caller can choose the photograph.
7. **The extension is a hint; compression is always stated.** `FormatInfo::container` comes from
   the file's leading bytes (`detect_container_format`) and the decoder's DNG version; the
   extension is recorded only as `extension_hint`. `FormatInfo::compression` is filled for every
   container: for DNG from the raw IFD's Compression tag (`Provenance::DngTag`); otherwise from
   LibRaw's codec and the recorded mode (`Provenance::Decoder`, `classify_vendor_compression`):
   codecs that are lossless or lossy by construction by the codec alone, Canon CRX by Quality,
   Nikon NEF by NEFCompression (1, 4, 13, 14 lossy; 3 lossless), Fujifilm compressed RAF by its
   header flag. A codec without an established rule keeps `Lossiness::Unknown` and is named
   after the codec; nothing known gives the name "unknown". `basis` records the evidence. Lossy
   data also lists `LossyCompression` in `prior_processing`.
8. **Required operations and interpretation are data.** Every DNG opcode of the decoded raw IFD
   becomes a `RequiredOperation` (stage, mandatory or optional, `supported_by_lucent` = false in
   W1, `applied_by_decoder` for LibRaw's lossy-DNG MapPolynomial). `Interpretation` lists issues
   and a derived status: `Complete`, `OptionalOperationsSkipped`, `RequiresUnsupportedOperation`
   (a mandatory opcode, or a DNGBackwardVersion above 1.7.1.0), `Ambiguous` (the DNG could not be
   inspected or the decoded image cannot be matched to exactly one raw IFD) or `NotDecodable`
   (LibRaw flags the format as undecodable, Fujifilm SuperCCD rotated layout, non-3-colour
   full-colour data). Also `RequiresUnsupportedOperation`: a Dual Pixel component frame, an
   unstitched dual-fisheye 360-degree source (`ViewLayout::DualFisheye360`, issue
   `MultiViewSource`: an Insta360 or Ricoh THETA body with a 2:1 or 1:2 frame, rule
   `classify_projection`), and DNG black-level deltas the contract cannot carry. A decode refuses
   only `NotDecodable` (as `Unsupported`); every other status travels with the data so that
   nothing downstream can call such an image verified. Processing already applied (demosaiced
   data, YCbCr conversion, as-shot white balance, DNG linearization, lossy compression of any
   container, the lossy-DNG tone curve) is listed in `prior_processing`.
9. **File access and source identity.** The adapter opens the file once with
   `core::ReadOnlyFile` (share mode READ|WRITE|DELETE, long and Unicode paths), captures its
   `FileIdentity` through the handle and feeds LibRaw through a private
   `LibRaw_abstract_datastream` over that handle. The stream reproduces LibRaw's Windows stream
   exactly (item-count reads, unclamped seeks, `get_char()` returning 0 at the end, the
   `gets`/`scanf_one` buffer rules) so that decoding matches LibRaw's own file path, and it records
   OS read errors, reads past the end and cancellation. When the call ends, the identity is
   checked again through the handle and the path (as `core::hash_file` does); any change is
   `SourceChanged`. Identity snapshots see size, last write time and file id only: writes
   through a memory-mapped view (which Windows does not reflect in the last write time) and
   writers that restore the timestamp are not detected (`core::hash_file` documents this; the
   W4 import pipeline decides whether to re-hash before publishing).
10. **Thread safety.** `libraw::raw_r`, linked PRIVATE to `lucent_raw`, with one heap-allocated
    LibRaw instance per call.
11. **Limits.** `DecodeLimits` = {`max_file_bytes` 2 GiB, `max_pixels` 2^28, `max_memory_bytes`
    2 GiB} (rationale in `raw_decoder.hpp`). The file size is checked before LibRaw sees the file;
    pixel count and estimated pixel memory (LibRaw's buffer plus Lucent's copy) after
    identification and before unpacking; `max_memory_bytes` is also LibRaw's `max_raw_memory_mb`.
    A probe enforces only the file size and reports dimensions.
12. **Cancellation.** The token is checked before work starts, in LibRaw's progress callback, on
    every read of the stream (which then returns no data and raises LibRaw's cancel flag), and
    every 256 rows of the copy. Any failure after cancellation was observed is `Cancelled`.
13. **Error mapping.** `libraw_error_code()` maps LibRaw codes (for example `FILE_UNSUPPORTED` ->
    `Unsupported`, `REQUEST_FOR_NONEXISTENT_IMAGE` -> `NotFound`, `DATA_ERROR` -> `Corrupt`,
    `IO_ERROR` -> `IoError`, `UNSUFFICIENT_MEMORY` -> `OutOfMemory`, `TOO_BIG` and
    `MEMPOOL_OVERFLOW` -> `LimitExceeded`, `CANCELLED_BY_CALLBACK` -> `Cancelled`). The adapter
    refines it: an OS read failure recorded by the stream wins; `IO_ERROR` without one is
    `Corrupt`; a failure to open a file whose signature Lucent does not recognise is
    `Unsupported`; a failure to open a recognised container whose structure extends beyond the end
    of the file is `Corrupt` at `raw.libraw.truncated` whatever LibRaw answered (LibRaw reports a
    DNG cut before its IFD 0 as `FILE_UNSUPPORTED`, which the registry would treat as "try the
    next decoder"); a failed open of an ORF, RW2, CR3 or RAF whose own structure is malformed is
    `Corrupt` at `raw.libraw.container` (a plain TIFF signature is shared by non-RAW images and
    keeps LibRaw's answer);
    LibRaw data errors after an otherwise successful call are `Corrupt` (as rawpy treats them); a
    decode whose white level is not above the highest black level is `Corrupt` at
    `raw.libraw.levels` (a property of the file, whereas `check_consistency()` failures are
    `Internal` adapter defects).
14. **Truncated files are rejected.** Before metadata is extracted, the container's declared
    structure is compared with the file size (`container_extent.hpp`), otherwise `Corrupt` at
    `raw.libraw.truncated`:
    - TIFF-structured files, including the vendor signatures ORF ("IIRO", "IIRS", "MMOR") and RW2
      ("IIU\0"), which are read as classic TIFF for this check only: every IFD of the IFD 0 chain
      and the SubIFD trees, and every strip and tile they declare, must lie inside the file. An
      IFD (IFD 0 included) whose plausibly sized entry table does not fit counts as truncation;
      one that lies inside the file but is malformed is skipped with a decoder warning (for a
      main-chain IFD the chain ends there), because LibRaw accepted the file and such an IFD is a
      writer defect, not missing data. Tables that cannot be interpreted are skipped with a
      warning; overruns found elsewhere are always kept. A SubIFDs list longer than the IFD
      budget (64) is read only up to that budget, so a crafted count cannot allocate memory.
    - CR3: every top-level ISO base media box must end inside the file.
    - RAF: the JPEG, CFA-header and CFA ranges of the RAF header must lie inside the file.
    During unpacking, more than 16 reads that start at the end of the file are also `Corrupt`;
    complete files may read ahead once (the OM System ORF fixture reads one 116 kB buffer past its
    end, which is reported as a decoder warning). The read-at-end rule alone missed ORF copies cut
    by 1 to about 45 bytes, which LibRaw decodes with wrong pixels in the last row; the ORF strip
    check now rejects them.
15. **Registry.** `DecoderRegistry` tries adapters in priority order; `Unsupported` passes the
    file on, any other error ends the selection. `create_default_registry()` holds the LibRaw
    adapter (priority 100). There is no third-party plug-in mechanism.
16. **Reference parity.** `DecoderInfo::reference_parity` carries LibRaw-model values with rawpy
    0.27.1 semantics (post-unpack sizes, `COLOR()` patterns, `adjust_bl_()` black levels,
    `cmatrix`), used only by `lucent-probe` and the fixture tests to compare with
    `fixtures/reference`. On 2026-09-25, after the review fixes, `lucent-probe --decode` and
    `research/tools/compare_probe.py` found all 34 fixtures matching field for field: 28
    single-channel files including the SHA-256 of the whole raw buffer (`--require-raw-hash`), 4
    full-colour ("stack") files, and the two Nikon HE files refused by both decoders. No parity
    allowance exists. `lucent-probe` also requires every step (stat, hash, probe, decode) to see
    the same `FileIdentity`, so `file_sha256` always describes the bytes the parity values come
    from.

## Consequences

- The engine (W2) receives a self-describing, decoder-independent input: it can choose Bayer,
  X-Trans, linear or monochrome routes from `RepresentationInfo`, subtract black itself from
  `SensorLevels`, and must refuse to report a file as verified unless `Interpretation` is
  `Complete`. Every DNG opcode currently makes a file `RequiresUnsupportedOperation` or
  `OptionalOperationsSkipped`; the DJI and Hasselblad drone fixtures are therefore not
  verifiable until W2+ implements GainMap, WarpRectilinear and FixVignetteRadial. The Insta360
  One X fixture is `RequiresUnsupportedOperation` (dual-fisheye source) until a stitching route
  exists.
- The inspector's strictness decides DNG interpretation. Running it on the corpus found one real
  writer quirk: the Pentax K-3 Mark III Monochrome stores BlackLevelRepeatDim as the single value
  1. The inspector now reads that one value as 1 x 1 with a warning (any other single value is
  still `Corrupt`); without it the file was `Ambiguous`.
- Copying costs one extra raw-frame buffer at peak (about 120 MB for a 61 MP mosaic) and a copy
  pass, in exchange for Lucent-owned, aligned memory whose lifetime does not depend on LibRaw.
- `DecoderRegistry::decode` probes and then decodes, so the file is identified twice; callers
  that already probed call the selected adapter directly.
- Truncation detection is structural for TIFF, DNG, ORF, RW2 (IFDs, strips, tiles), CR3
  (top-level boxes) and RAF (header ranges). It does not cover data that none of these declare:
  a DNG whose last bytes are out-of-line values of IFD 0 (the Insta360 One X fixture cut by 2
  or 17 bytes), or an RW2 (whose raw data has no strip table) cut in its trailing bytes, still
  decodes; in the measured cases the pixel data was complete. Files with other signatures rely
  on LibRaw's checks and the read-at-end threshold.
- The compression and subimage rules read LibRaw maker-note fields and, for Fujifilm, Panasonic
  and CR3 track layout, LibRaw's unpacker state. That state is not a stable LibRaw API: the
  fixture tests (`LibRawFixtures.CompressionModesAreDistinguished`,
  `DualPixelFileRequiresAnExplicitSubimage`) pin the extracted facts and must pass after every
  LibRaw upgrade.
- `ReferenceParity` is decoder-specific diagnostics inside a decoder-agnostic contract. It is
  optional, never used for processing, and would be omitted by a future non-LibRaw adapter.
- Floating-point DNG levels come from the DNG tags (LibRaw's integer level fields cannot hold
  fractional values). This path is covered by synthetic tests only (with and without
  WhiteLevel); no real floating-point DNG is in the corpus.
- Not handled in W1: stitching or projection-aware processing of 360-degree sources (they are
  only recognised, for the known two-lens bodies), already stitched equirectangular panoramas
  (XMP GPano metadata is not read; such a DNG is reported as `SingleView`), per-row/per-column
  black levels (flagged, not represented), Fujifilm SuperCCD (refused), 4-colour full-colour
  data (refused), per-subimage DNG IFD matching (reported as `Ambiguous`), Dual Pixel processing
  and pixel-shift combination (the frames are labelled, not processed).
- Revisit when LibRaw is upgraded (re-run `lucent-probe` against `fixtures/reference`), when the
  Adobe DNG SDK or another adapter is added, or when the capture-time conversion (LibRaw uses
  the C runtime's local-time `mktime()`; Lucent converts back with `localtime_s`, which can be one
  hour off inside a daylight-saving gap) needs a time zone.

## Alternatives considered

- **`LibRaw::open_file(const wchar_t*)`.** Available, and what rawpy uses. Rejected: it blocks
  writers (FILE_SHARE_READ only), fails beyond MAX_PATH, loses the OS error, and reads a file
  object whose identity Lucent did not capture, so the decoded bytes could not be tied to a
  `FileIdentity`. The datastream keeps LibRaw's exact read semantics, and parity with rawpy is
  unaffected (all 34 fixtures match).
- **`LibRaw::open_buffer` over the whole file in memory.** Simple, but doubles memory for large
  files and still needs a stable read; rejected.
- **Zero-copy views of LibRaw's buffers.** Would keep the LibRaw instance (and its other
  allocations) alive as long as the pixels, tie pixel lifetime to a third-party object and expose
  unaligned rows; rejected.
- **Black subtraction or normalisation in the adapter.** Would bake one decoder's black model
  into the data and lose masked-pixel information; rejected. The engine owns sensor
  normalisation (blueprint section 5, step 2).
- **Default to the first subimage.** Explicitly forbidden by the blueprint; rejected.
- **Letting LibRaw rescale floating-point DNG data to integers.** Lossy and undocumented scaling;
  rejected (Lucent differs from rawpy's default here; no fixture is affected).
- **Treating any read past the end of the file as corruption.** Rejected after measurement: a
  complete OM System ORF reads one buffer past its end.
- **Refusing files with unsupported mandatory opcodes at decode time.** Rejected: the raw data is
  valid and useful for diagnostics and future processing; the refusal belongs to whoever would
  publish a verified result, so the requirement is carried as data.
