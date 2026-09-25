# Lucent — Desktop RAW Processing Blueprint

**Revision 3 · 23 September 2026**  
**Product:** an offline, camera-independent desktop application for natural photographic development, automatic batch processing, and precise individual editing.  
**First platform:** Windows 11 x64; an ordinary CPU laptop with approximately 16 GB RAM. GPU acceleration is optional.  
**Deliverable described here:** an installed graphical application launched by double-clicking **Lucent**, with no terminal, Python installation, or developer tools required.

Lucent should let a photographer select photographs or an input folder, select an output folder, and obtain a separately optimized, editable result for every supported image. Opening one photograph provides the same processing engine with detailed controls. Originals remain untouched.

This revision replaces the Canon-specific application design with a capability-based camera system, a native desktop architecture, durable batch processing, and explicit installation and quality gates. It preserves the researched algorithm and repository evidence in the appendices. Camera and packaging sources were checked on 23 September 2026; historical paper citation counts and repository snapshots retain their original 20 September date.

**Evidence boundary:** this is a researched implementation blueprint, not a completed application or a measured algorithm comparison. No photographer RAW files were supplied, and no model replication, camera qualification, installer test, or performance benchmark was run for this update. “Selected” means the recommended implementation direction; “verified camera support” must be earned with tests.

**Navigation:** [Workflow](#2-the-photographers-workflow) · [Camera support](#3-camera-compatibility-is-a-capability-contract) · [Architecture](#4-native-application-architecture) · [Pipeline](#5-one-explicit-image-processing-pipeline) · [Automatic settings](#6-automatic-settings-for-every-photograph) · [Batch reliability](#7-durable-memory-bounded-batch-processing) · [Forward compatibility](#8-recipes-assets-and-forward-compatibility) · [Windows delivery](#9-double-click-windows-delivery) · [Validation and roadmap](#10-validation-release-gates-and-roadmap) · [Papers](#appendix-a-algorithm-and-paper-evidence) · [Repositories](#appendix-c-repository-readiness-and-licensing) · [Reproduction](#appendix-d-reproduction-and-implementation-handoff)

## 1. Product decisions

| Area | Decision | Why |
|---|---|---|
| Main application | C++20 engine with Qt 6 Quick/QML interface | One native desktop product, direct access to imaging libraries, and a responsive UI separated from expensive work |
| RAW decoding | LibRaw behind a replaceable adapter | Broad existing camera coverage without coupling the product to a single brand |
| Research environment | rawpy, Python, and the original paper implementations | Fast experiments and numerical references; these are development tools, not end-user prerequisites |
| Automatic settings | A new constrained per-image optimizer; Exposure provides the explicit-edit research reference | Produces editable exposure, white-balance, tone, and color settings for each image |
| Learned appearance | Image-Adaptive-3DLUT as an optional, validated proposal/appearance stage | A small analysis network selects a global transform that is inexpensive to apply at full resolution |
| Local tone | Guided Image Filtering as the first edge-aware primitive | A reproducible foundation for bounded luminance adjustments and mask refinement |
| Color and optics | Little CMS for ICC transforms; Lensfun when an appropriate profile exists | Explicit color management and reusable lens-correction infrastructure |
| Persistence | Local SQLite catalog, immutable recipe revisions, durable job records | Restartable batches, reversible edits, and traceable exports |
| Inference | ONNX Runtime CPU deployment where reference parity is demonstrated | Offline inference without requiring a particular GPU; export from a research model is a validation task |
| Delivery | Signed graphical installer and installed `Lucent.exe` | Normal Windows installation, shortcuts, and double-click launch |

There is no defensible single “best RAW algorithm” covering decoding, demosaicing, denoising, white balance, aesthetic editing, and export. The selected system combines specialized methods. The most important new component is the automatic controller; neither Exposure nor the LUT repository supplies Lucent's complete editor, scheduler, or quality policy.

The release must work automatically even if learned components do not pass quality or redistribution gates. A conservative deterministic optimizer is the production baseline. Enable a learned method by default only when it improves held-out photographs from the supported camera families without increasing serious defects.

### Scope of the first release

- Offline import, browsing, RAW-derived previews, camera/mode diagnostics, automatic batch development, and detailed individual editing.
- Independent per-image recipes, optional consistency among related shots, resumable exports, and a useful completion report.
- JPEG and 16-bit TIFF export with embedded output profiles; proprietary RAW files are never overwritten.
- Qualification targets include Canon, Sony, DJI, and conventional Insta360 RAW stills, followed by more models and brands through the same architecture.
- HDR merging, burst denoising, focus stacking, video processing, and native multi-lens 360 stitching are separate later workflows. They must not be silently applied to an ordinary batch.

## 2. The photographer's workflow

### Import and organize

The opening screen has **Add photos**, **Add folder**, and a recent-library list. Native Windows file/folder pickers and drag-and-drop are supported. Folder import offers an explicit **Include subfolders** option and displays the selected source location.

Default import references files in place. An optional **Copy into library folder** choice lets the user select a destination, verifies each copy before registering it, and leaves the source intact. A removable-card source remains visibly identified so the photographer can copy or relink it before removal. Missing files remain in the catalog with a relink action.

Discovery excludes Lucent's cache, temporary exports, and selected output tree. Canonical path checks prevent recursive reimport when an output folder is nested under the input; symbolic links/junctions cannot create scan loops. Different filenames with identical contents are detected, while intentional duplicates can remain separate library items.

The library is a contact sheet with camera/model, capture time, processing status, support status, and flags. An embedded camera JPEG may appear immediately for browsing, labeled until replaced by Lucent's own render. It must not be used as the RAW optimization input or imply exact preview/export agreement.

### Automatic batch development

1. Select photographs, an album, or an input folder.
2. Choose **Natural** rendering, optional look strength, and whether related photographs should be kept visually consistent.
3. Choose an **output folder**, JPEG/TIFF preset, naming pattern, metadata preference, and collision policy. Show example destination paths before starting.
4. Click **Auto-process batch**. Lucent analyzes each image, chooses its own settings, validates the result, and exports automatically.
5. Show completed/remaining counts, current stage, estimated time when enough evidence exists, pause/cancel controls, and actionable flags. Finish with **Open output folder** and a report of exported, skipped, failed, and review-recommended items.

The user does not have to approve each image. A low-confidence aesthetic decision produces a restrained result and a review flag. An undecodable file or an image missing a mandatory interpretation step is skipped with a specific explanation; other files continue. An optional **Analyze only** mode produces editable previews before export.

### Detailed individual editing

Double-clicking an image opens a dedicated editing view within the same application. Include a filmstrip, fit/100% zoom, before/after comparison, histogram, clipping overlays, undo/redo, and reset to as-shot or an earlier recipe.

Provide exposure, temperature/tint where calibration permits, shadow/highlight controls, a monotone tone curve, contrast, saturation, lens corrections, crop, rotate, straighten, and export sharpening. Provide brush, linear-gradient, and radial masks with feathering for detailed local work. These masks do not require an unvalidated semantic segmentation model.

When an accurate Kelvin conversion is unavailable, preserve and expose camera-neutral/tint adjustments without inventing a physical temperature. Noise reduction starts with the guided chroma-noise method specified in section 5; advanced luminance and learned RAW denoising are separately qualified upgrades.

**Auto** creates a new editable revision. The photographer can lock controls before rerunning it. The optimizer may change only unlocked controls and must report when the locks prevent its usual technical targets. Copy/paste adjustments offers specific categories; copying a whole recipe across cameras must not copy camera calibration, sensor black levels, or camera-specific white-balance gains.

Detailed editing and batch export share the same operation definitions and renderer. Editing a picture during a batch creates a new revision; an already queued export retains its frozen revision until the user explicitly replaces or requeues it.

### Interface character

Use a restrained neutral interface with generous image space, clear typography, keyboard navigation, accessible contrast, and a compact control hierarchy. **Library**, **Edit**, and **Exports** are three views of one application. Display photographic controls and useful outcomes; keep worker processes, model formats, and pipeline diagnostics inside an optional diagnostics panel.

## 3. Camera compatibility is a capability contract

### Support files and capture modes, not just brand names

Use LibRaw's [supported-camera list](https://www.libraw.org/supported-cameras) as an upstream capability inventory. It is not Lucent's quality certification. Record the actual decoder version, enabled codecs, file representation, compression mode, camera firmware when available, and required transforms. The [LibRaw changelog](https://github.com/LibRaw/LibRaw/blob/master/Changelog.txt) shows why this matters: support can depend on optional components and exact file modes.

File extensions are hints. A `.DNG` may contain a Bayer mosaic, another CFA, already demosaiced linear data, or specialized image content. Sony variants can have different layouts or prior processing; LibRaw documents metadata indicating processing already applied. Never assume that all RAW data is an untouched Bayer array. [LibRaw data structures](https://www.libraw.org/docs/API-datastruct-eng.html)

| Qualification family | Initial target samples | Required distinctions |
|---|---|---|
| Canon | EOS R5 RAW and C-RAW; at least one additional CR2/CR3 model | Compression, dimensions, bit depth, crop mode, lens, orientation; do not promise Dual Pixel processing merely because ordinary RAW decoding works |
| Sony | A7 IV and a representative A7R model | ARW uncompressed, compressed, and available lossless sizes; any prior WB/color processing; unsupported combinations remain explicit |
| DJI | Mini 4 Pro and a representative Mavic camera | DNG at each supported resolution, camera/lens module, metadata corrections, and any required DNG operations |
| Insta360 planar stills | Ace/Ace Pro/Ace Pro 2 DNG fixtures, starting with one exact model | Ordinary single-view RAW development, lens geometry, resolution, and file mode; brand inclusion is not a promise to stitch every 360 file |
| Nikon | Representative NEF | Standard versus high-efficiency compression and the actual enabled decoder capabilities |
| Fujifilm | Representative X-Trans RAF and Bayer RAF | CFA layout, compression, and separate handling of multi-shot/pixel-shift files |
| Other brands / generic DNG | Panasonic, OM System, Pentax, Leica, Hasselblad, and other upstream-supported fixtures as acquired | Validate each model/mode rather than infer support from the brand or suffix |

These are planned tests, not a claim that Lucent already supports these models. Sony documents its [A7 IV RAW mode variants](https://helpguide.sony.net/ilc/2110/v1/en/contents/TP1000659395.html); DJI documents [Mini 4 Pro still formats and resolutions](https://www.dji.com/mini-4-pro/specs); Insta360 documents [Ace-series RAW/DNG stills](https://onlinemanual.insta360.com/acepro2/en-us/specs/shooting).

### Insta360 and 360 imagery

Treat conventional Insta360 DNG stills as a legitimate v1 qualification target. Separately identify multi-lens source images, INSP files, already stitched panoramas, and INSV video. The [Insta360 file-format guide](https://onlinemanual.insta360.com/x4air/en-us/operating-tutorials/storage/file-format) distinguishes these representations; its [Pro RAW workflow](https://onlinemanual.insta360.com/pro1/en-us/photo/postproduction/1) includes stitching.

Native 360 stitching requires lens calibration, projection, seam handling, and often parallax decisions. LibRaw does not supply that workflow. Version 1 should recognize unsupported 360 sources and explain the required preparation instead of silently exporting a distorted image or one lens. Already stitched 360 editing is a later projection-aware path: preserve panorama metadata, use horizontal wraparound for filters and masks, and disable inappropriate rectilinear corrections. Ordinary planar TIFF/JPEG import can be supported separately without claiming that it restores original sensor RAW information.

### User-visible compatibility states

| State | Meaning and behavior |
|---|---|
| Verified | Exact model and relevant capture mode passed Lucent's decoding, color, geometry, and export tests |
| Compatible, unverified | Decoder and required interpretation steps succeed, but Lucent lacks representative quality fixtures; show the status and permit conservative processing |
| Limited | A known optional feature or profile is unavailable; describe the limitation and available conservative path |
| Unsupported | Decoding or a mandatory interpretation step is unavailable or ambiguous; skip automatic export and give a useful reason |

An optional lens-profile miss can leave correction off. A missing required color interpretation, unsupported CFA, or mandatory DNG opcode cannot silently become a verified image. Unknown future models may work through a known representation and valid metadata, but future compatibility is an update strategy, not a guarantee about every future camera.

### Adapter boundary

Define a versioned `RawDecoder` interface with `probe`, `decode`, cancellation, limits, and structured error reporting. Its result carries:

- Source identity and subimage index; format, compression, camera/lens identifiers, dimensions, active area, orientation, and image count.
- `MosaicFrame`, `LinearColorImage`, or `MonochromeFrame`; integer/float encoding, CFA layout, channel mapping, and any processing already applied.
- Black/white levels, channel-dependent values and spatial corrections where provided, as-shot neutral, camera-to-reference color data, illuminants, and noise metadata with provenance.
- Required gain maps/opcodes, geometric corrections, projection, calibration confidence, warnings, and the decoder/build capabilities used.

Do not silently select the first image in a multi-image RAW. Expose supported subimages or explicitly reject the mode. Preserve distinct sensor channels, including two green channels when relevant, until the selected processing path permits their combination.

Use an internal adapter registry first. Adding a novel codec or CFA path requires a tested engine release; a data-only camera profile update cannot implement a new compression format. Avoid a general third-party native-plugin system in v1.

## 4. Native application architecture

```text
Qt Quick interface: Library / Edit / Exports
                 |
Application services: import, recipes, catalog, export, diagnostics
                 |
Local SQLite + asset registry + job scheduler
                 |
Versioned worker protocol and cancellation
                 |
Isolated native worker processes
  decoder adapter -> typed RAW/linear data -> shared render graph
  analysis/optimizer -> frozen recipe -> output encoder + verification
```

| Module | Responsibility | Dependency rule |
|---|---|---|
| `ui` | Windows, accessibility, image presentation, editing controls | Calls application services; never decodes RAW on the UI thread |
| `application` | Import/export use cases, command history, recipe revisions | Owns workflow; does not implement pixel algorithms |
| `catalog` | Local database, migrations, source location/relinking | Stores identities and state, not mutable pixel buffers |
| `engine` | Typed image graph, operation semantics, color and geometry | Independent of QML and window objects |
| `raw_adapters` | LibRaw and future decoders | Converts decoder-specific state into the explicit data contract |
| `analysis` | Metrics, deterministic proposals, optional learned inference | Uses engine operations rather than a second renderer |
| `workers` | Process lifecycle, memory admission, job isolation | One failed file does not bring down the UI or whole batch |
| `export` | Encoding, profiles, metadata, verification, publication | Receives immutable recipe/output specifications |
| `assets` | Camera/lens profiles, models, licenses, hashes | Versioned manifests; no silent resource replacement |

Keep decoding, optimization, and encoding off the GUI thread. Use asynchronous tasks and a worker process for crash containment and release of native allocations. Process separation alone is not a security sandbox. Set file-size/dimension/allocation limits, validate messages, and stop or restart a worker that exceeds its job timeout or becomes unresponsive.

Each preview request carries image ID, recipe revision, geometry revision, quality level, and generation token. A late result from an earlier slider position must never replace the current preview. Debounce adjustments, display a quick preview, then refine. Give interactive jobs reserved capacity so a background batch cannot make detailed editing unusable. [Qt Quick performance guidance](https://doc.qt.io/qt-6/qtquick-performance.html)

The UI and worker negotiate a protocol version. Package matching binaries together; fail clearly if incompatible components are mixed. Keep the last stable UI state and a recoverable job record when a worker fails.

## 5. One explicit image-processing pipeline

### Sensor interpretation and rendering

1. **Inspect and decode.** Validate the representation, choose a supported adapter, preserve metadata and the original hash, and obtain raw/linear data. Disable undocumented automatic brightening in reference experiments.
2. **Normalize sensor data.** Apply the correct black/white levels, active-area rules, required sensor corrections, and supported defect handling. Track source saturation before resampling. Preserve meaningful out-of-range values internally.
3. **Choose the reconstruction route.** Bayer uses a qualified demosaicer; X-Trans uses its own supported route; linear RGB bypasses demosaicing; monochrome bypasses color reconstruction. Do not force AHD or a Bayer network onto every layout.
4. **Correct optics in the appropriate sensor/color domain.** For the initial conventional Bayer route, use linear camera RGB for profile-based vignetting and channel-specific lateral chromatic aberration correction, combine geometric resampling where practical, and then transform camera color. Required DNG operations follow their specified stage/order instead of this generic route. Record the geometry mapping, normalize orientation consistently, and never apply the same DNG/vendor correction again through Lensfun.
5. **Complete white balance and camera color interpretation.** Honor operations already applied by the decoder/file and white balance already applied before reconstruction. Use a documented camera-profile transform into a reference space, chromatic adaptation where needed, then the common working space. Avoid applying white balance twice.
6. **Apply photographic operations.** Exposure, bounded local luminance changes, tone curve, color adjustments, masks, and any compatible learned transform use the frozen recipe. Denoising and sharpening have explicit stages and validated interaction with these operations.
7. **Render for a destination.** Apply the destination tone/gamut treatment, resize if requested, and apply output sharpening at the requested size. Convert into the selected output profile and quantize only at encoding.
8. **Encode and verify.** Embed the profile, write selected metadata, validate the written result, and publish it using the durable export protocol.

This is a logical contract. Individual algorithms may require white balance before demosaicing or a particular correction before reconstruction. Record the exact ordered graph and operation semantics for each route; do not let libraries silently choose a different order. Orientation, active-area cropping, and lens transforms must map sensor diagnostics and masks consistently.

**Initial demosaicing decision:** stock LibRaw AHD is a deterministic Bayer baseline, not a universal quality winner. Reproduce Malvar–He–Cutler as a numerical reference. Compare difficult foliage, rock textures, stars, and repeating detail against AMaZE/RCD renderings from RawTherapee before locking the final choice. Its GPL implementations are quality references unless a compatible distribution decision is made. Non-Bayer routes need separate acceptance tests.

### Color and model contracts

Use float32 scene-linear **Rec.2020 primaries / D65** as the initial common working-space design, with an explicit camera-to-reference transform and adaptation. Negative or above-one values may exist internally; do not clip them merely to fit a tensor. Working gamut does not create colors or dynamic range absent from the source. Implement and verify matrix, transfer-function, and ICC conventions with numerical references. Little CMS handles ICC conversions; it does not invent an accurate input camera profile.

Each graph edge declares channel order, color primaries, white point, transfer function, range, alpha/mask convention, orientation, and scene/display reference. The current display ICC profile applies only to the screen branch. Export always uses the selected embedded output profile and must remain stable when the monitor changes.

The selected LUT repository has distinct sRGB and XYZ experiments. Its sRGB checkpoint is a **rendered sRGB-to-sRGB** method, not a RAW decoder or a scene-linear wide-gamut model. Exposure uses a different linearized ProPhoto input convention. Never connect either directly to camera RGB or substitute one model's preprocessing for another's.

For v1, expose the validated sRGB LUT only inside an explicit sRGB appearance route. A TIFF derived from this route does not regain wide-gamut information by being tagged with a larger profile. A genuine wide-gamut TIFF route must use a compatible validated transform or omit the learned LUT, show that route in preview, and store the choice in the recipe before export. Do not silently switch appearance when the user selects TIFF.

A model manifest records input/output domains, training-domain assumptions, resize/crop behavior, channel order, numerical range, preprocessing, model and weight hashes, runtime/opset, execution provider, and allowed pipeline placements. A missing/incompatible model selects the declared non-learned fallback when creating a new recipe; replay of an existing recipe reports the missing dependency instead of silently changing its result.

### Noise, masks, and geometry

The first noise-reduction control is **guided chroma denoising**, built from the selected [OpenCV guided-filter implementation](https://github.com/opencv/opencv_contrib/blob/4.x/modules/ximgproc/src/guided_filter.cpp). After reconstruction and camera-to-working conversion, before local tone, learned appearance, and sharpening, derive luminance and two color-difference planes from linear working RGB. Filter the chroma planes with luminance as the guide, reconstruct RGB, and blend conservatively. Preserve the luminance plane; this control does not claim to remove all luminance noise or replace a camera-qualified RAW denoiser.

Define radius in full-image pixels and regularization epsilon in the guide's declared squared signal units. Set strength from a validated camera/noise estimate or a conservative image-derived estimate; low-confidence estimates choose zero or minimal strength. Record parameters and the operation version in the recipe. This particular denoising policy is Lucent engineering built on the filtering primitive, not a pretrained feature supplied by its repository. Compare against no denoising and inspect colored fine detail before enabling it automatically. Keep a no-denoising option. Do not transfer a noise profile from one camera merely because ISO values match. Neural RAW denoising requires camera/CFA/noise validation and separate rights to distribute weights; it is not a default v1 dependency.

Store local masks in a defined normalized image coordinate system tied to the orientation and geometry revision. Transform masks predictably through lens correction, crop, rotation, and resizing. Distinguish scalar edit masks from channel-aware sensor validity maps. Test that reopening a recipe or changing a crop cannot move a brush stroke to a different mountain or sky region.

Guided-filter radius and local-operation scale are defined relative to full-image coordinates. Derive preview radii from that definition. Downsampling and filtering do not generally commute; specify preview/export tolerances and validate native-resolution crops. Tiled processing requires enough halo for the entire composed operation, not only one filter pass.

## 6. Automatic settings for every photograph

### Meaning of “optimal”

Lucent chooses the best evaluated candidate that meets calibrated technical constraints and the selected photographic preference. It does not claim a global mathematical optimum or a universally correct artistic edit. The policy should preserve blue hour, warm sunsets, fog, snow, deep shadows, and deliberate silhouettes instead of forcing every image toward a middle-bright neutral look.

The first policy is **Natural v1**: small plausible corrections, restrained color, protected highlight gradients, and limited noise amplification. Source uncertainty reduces edit strength. A semantic scene classifier is optional research; do not depend on an unvalidated sky/sunset label for safe processing.

### Analysis and candidate search

1. Decode once per active image and derive sensor saturation/noise diagnostics. Generate a 1536-pixel-long-edge RAW-derived analysis image and a 512-pixel search proxy while preserving aspect ratio.
2. Establish the neutral/as-shot baseline, histogram/headroom proposals, evidence-supported white-balance proposals, and optional validated learned parameter proposals. Keep baseline and identity candidates in the set.
3. Run bounded deterministic coordinate search or a small beam over explicit operations. An initial budget of approximately 24–32 cheap proxy candidates is a tuning starting point, not a throughput claim. Respect locked user controls.
4. Rank feasible candidates using the calibrated metric contract below. Recheck only the leading three at analysis resolution and on deterministic native-resolution crops covering detail, shadows, highlights, and strong edges.
5. If enabled, regularize related-shot upstream settings and revalidate them before learned appearance inference. Then, if domain-compatible, infer the adaptive LUT from that chosen upstream rendering once, compare its permitted strengths, and validate the combined result. Never infer a different LUT for each export tile or zoom level.
6. Optional consistency of LUT strength may reuse the same LUT. Any subsequent change to upstream exposure, white balance, tone, color, or model-input geometry invalidates it and requires inference again. Revalidate each image, then freeze the full recipe, policy/search versions, model input identity, LUT or its reproducible assets, and diagnostic results.
7. Render the selected recipe once at full resolution. If final checks fail, try a bounded conservative fallback and recheck. Every fallback that changes image operations creates and freezes a new recipe revision, retains the previous attempt and reason, and binds export identity/receipt to the effective revision. If still invalid, mark the file failed; do not loop indefinitely or block the rest of the batch.

The optimizer, objective calibration, camera routing, and confidence system are **new Lucent engineering work**. Exposure is a research reference for explicit photographic operations and learned proposals, not an existing implementation of this search loop.

| Automatically chosen setting | Initial conservative search bounds | Qualification requirement |
|---|---|---|
| Exposure | −1.5 to +1.5 EV relative to the baseline | Highlight headroom and intentionally low/high-key scenes |
| White balance | As-shot plus small supported residual changes; initial red/blue gain bound ±10% in the route's defined neutral representation | Do not reuse sensor gains across different cameras; preserve intentional illumination |
| Shadows | 0 to +0.75 EV bounded local lift | Reject harmful noise amplification or flat, unnatural local contrast |
| Highlights | −0.75 to 0 EV local compression | Preserve smooth transitions; no invented recovery of fully clipped channels |
| Contrast | A monotone curve with a narrow initial parameter range around identity | No contrast reversals, halos, or blocked shadows |
| Saturation | Initial multiplier 0.85–1.15 in a specified color representation | Neutral stability, hue behavior, and output gamut |
| LUT appearance | 0, 0.25, 0.5, or 0.75 strength after domain checks | Identity remains eligible; a learned model must beat conservative alternatives |
| Lens corrections | Metadata/profile-supported choices only | Exact match or a declared limitation; never a guessed profile |

These are prototype search bounds, not measured universal safe limits, physical slider definitions, or limits on deliberate manual editing. Calibrate them by scene/camera domain; record the policy version.

### Metric contract: hard checks before aesthetic ranking

Do not ship an arbitrary weighted sum of unrelated unnormalized quantities. Define each metric's domain, mask, units, baseline, failure threshold, and missing-data behavior. Calibrate normalization and preference weights on training/validation sets, then freeze them before testing on held-out cameras and trips.

| Metric | Defined measurement | Use |
|---|---|---|
| Numerical validity | NaN/Inf counts, dimensional consistency, valid transforms, monotonicity where required | Hard failure; never an aesthetic tradeoff |
| Source saturation | Per-sensor-channel counts and masks before reconstruction, with acquisition thresholds recorded | Report an acquisition limitation rather than blame the edit |
| Render-induced clipping | Newly clipped output samples relative to the baseline in the same declared output encoding; separately report all output clipping | Reject excessive new damage; do not hide it by excluding a large propagated mask |
| Brightness and tonal distribution | Luminance quantiles and headroom relative to the baseline and a calibrated scene/style policy | Preference feature, not a requirement that every histogram look alike |
| White-balance plausibility | Residual neutral/chromaticity change plus evidence confidence in the common color representation | Restrain unsupported changes; unavailable neutral evidence lowers confidence |
| Shadow noise | Estimated noise amplification and measured native-crop texture/noise indicators | Limit aggressive shadow lift and destructive denoising |
| Halos and detail | Edge overshoot and local-detail changes on fixed native crops at a defined scale | Guard strong local contrast, demosaicing artifacts, and sharpening |
| Color/gamut | Hue/chroma changes, neutral drift, and out-of-gamut fraction in a specified reference/output space | Restrain color damage; separate output-gamut limits from sensor limits |

Track valid-sample counts. A metric with too little evidence cannot receive an artificially good score; it triggers a smaller edit or a quality flag. Report source clipping, total output clipping, and newly induced clipping separately. Propagated sensor masks support interpretation but cannot exempt broad regions from inspection.

Among technically acceptable candidates, rank using a calibrated photographic preference model or normalized heuristic objective. Prefer the smaller edit when scores are effectively tied. Generic no-reference image-quality scores are diagnostics, not proof that an edit matches the photographer's intent.

Manual locks apply throughout candidate search, group consistency, fallback, and final quality retries. An intentionally clipped or strongly colored manual edit can remain numerically valid: export it with a relevant review flag instead of silently undoing it. A mandatory interpretation failure or invalid numerical result still fails explicitly. Do not change locked controls to make an automatic score look better.

### Related-shot consistency

Group conservatively using capture time and visual similarity, with user override. Treat consistency as a soft penalty on appearance, never a shared exposure setting for the whole folder. Large illumination changes split a group. Within a known camera/calibration domain, residual WB changes may be compared; across cameras compare color-managed neutral/chromaticity and rendered appearance instead of averaging camera gains. Locked/manual images can serve as anchors only with explicit user selection.

## 7. Durable, memory-bounded batch processing

### Job lifecycle

```text
Discovered -> Probed -> Analyzed -> Recipe frozen -> Queued
           -> Rendering -> Verifying -> Publishing -> Complete
                 |             |
                 +-> retry / conservative fallback / Failed
Any pre-publication stage -> Paused or Canceled at a safe boundary
```

Store source identity, subimage, recipe revision, destination specification, attempt count, lease/heartbeat, and stage in local SQLite. Use transactions and a single coordinated writer. SQLite WAL supports concurrent readers, but keep the live database on a local disk, not an SMB/network share. [SQLite WAL documentation](https://www.sqlite.org/wal.html)

Persist a job before dispatch. On restart, reconcile incomplete jobs with output receipts and temporary files. Do not mark an abandoned worker lease complete; safely retry from the latest valid stage. Bound transient retries separately from deterministic decoder/format failures. Disk-full, removed drive, permission loss, and missing source states must offer clear recovery actions.

Pause stops dispatch and lets a current safe stage finish; cancel stops pending jobs and requests cooperative worker cancellation. An unresponsive worker can be terminated after its timeout, with unpublished temporary output cleaned up. Already completed outputs remain valid.

### Memory and scheduling

Use an aggregate memory budget, not “one worker per CPU core.” A 45 MP RGB float32 buffer alone is roughly 540 MB in decimal units; raw arrays, intermediate buffers, masks, inference, and library scratch allocations multiply that cost. Estimate from actual dimensions, representation, graph, and measured overhead, including decoder allocations outside tile control.

For a 16 GB reference laptop, begin with an approximately 8 GB total Lucent budget including UI/cache and a reserve, then adapt to available system memory. Start with one full-resolution worker. Admit a second only after measurements show both fit comfortably. Very large files may force a single worker, lower caches, or a clear insufficient-memory result; do not silently reduce export resolution.

Limit decoder/OpenCV/inference thread pools to avoid nested oversubscription. Reserve interactive preview capacity and pause background admission when detailed editing needs memory. Use tiled/striped operations where the algorithm supports them, with validated halo handling; do not claim LibRaw decoding becomes streaming merely because export uses tiles.

Cache previews, analysis features, and reusable intermediates under a user-configurable size limit. Cache keys include source/subimage hash, decoder build, operation versions, calibration assets, recipe, geometry, and output domain. Cached data is disposable; catalog records and edits are not. No full-resolution float cache for every photograph in a large batch.

### Export integrity and user-selected destinations

Provide **JPEG / sRGB / quality 95** as a starting sharing preset and **16-bit TIFF / selected qualified ICC profile** as an editing preset. Show whether a chosen appearance route has already limited gamut. Preserve useful capture EXIF and copyright; provide an explicit GPS include/remove option. Do not copy embedded camera thumbnails as if they represented the edited result.

Use names such as `{original-stem}_Lucent_{revision}` with optional subfolders mirroring the input. Validate Windows names, long/Unicode paths, and case-insensitive collisions. Default to creating a unique new filename; never overwrite an original. Test writable destination and free space before the batch, and handle changes during it. Network/removable destinations are permitted as export targets with their weaker durability reported and handled; the live catalog remains local.

Define a job identity from source content hash + subimage + recipe revision/hash + renderer and asset versions + export settings. Detect source modification with stable-read checks before/after hashing and decoding; reimport a changed source instead of using stale diagnostics. File path alone is not image identity.

Write to a temporary sibling in the destination filesystem. Close/flush, reopen with the relevant decoder, and verify dimensions, profile, basic metadata, and image integrity before final publication. Default publication uses a no-clobber same-filesystem primitive: a file created after the initial collision check cannot be overwritten. Recheck destination identity and reserve a new name if necessary. Replacement is permitted only under the user's explicit overwrite policy for that destination, with identity revalidated at publication; originals remain prohibited targets. Persist a receipt with output hash and settings. Do not promise transactional atomicity across an image, sidecar, and database: use a publication journal and reconcile interruptions between each step. On restart, verify an existing output against its receipt before counting it complete.

Sidecar failures are visible and recoverable. Keep the authoritative recipe in the catalog and offer a portable Lucent JSON sidecar alongside exports; do not label it interoperable XMP without an implemented schema mapping. A report must identify any missing companion file instead of claiming the whole job succeeded.

## 8. Recipes, assets, and forward compatibility

An immutable recipe contains source/subimage identity, schema version, ordered operation IDs and semantic versions, user/automatic origin, parameters/locks, working/output color contracts, geometry/masks, decoder build and enabled features, camera/lens calibration, policy/search version, model and LUT hashes, deterministic seed if used, and export intent. Avoid a single opaque “auto enhanced” flag.

Version these contracts independently: catalog schema, recipe schema, operation semantics, worker protocol, model manifest, and camera/profile database. Migrations run transactionally after backing up the catalog and have explicit compatibility tests. Never interpret a newer unknown operation as a no-op. Open incompatible recipes read-only with a clear reason, or offer an explicit upgrade that creates a new revision.

Reproducibility means the same declared pipeline and assets within documented numerical tolerances. CPU and GPU paths may not be byte-identical. Retain resource hashes and, where practical, a compatible renderer version for old recipes. If an exact old renderer/model is unavailable, say so and create a reviewed migration path; never promise arbitrary future versions will reproduce every historical pixel exactly.

New supported camera profiles can arrive as versioned data packs when the existing engine already understands the file. New codecs, security fixes, or new operation types ship as signed application updates. Version 1 uses a signed full installer and a graphical **Check for updates** flow, with a complete offline installer also available. Verify downloaded installers and standalone profile/model packs against an authenticated release manifest or signature tied to the trusted publisher identity before activation; hashes alone do not authenticate a download. Apply application updates between jobs and retain migration recovery information. No cloud account or network connection is required for opening, editing, or exporting.

Keep resources separate:

| Location | Contents |
|---|---|
| Installed application directory | Executables, Qt/runtime DLLs, read-only bundled profiles/models, licenses, asset manifest |
| Per-user application data | Local catalog, settings, recipe records, job journal, installed asset versions |
| Per-user cache directory | Disposable thumbnails, proxies, temporary analysis data, bounded logs |
| User-selected source/library folders | Referenced or explicitly copied originals; never silently moved or rewritten |
| User-selected export folders | Completed images, optional sidecars, batch report, short-lived publication files |

The application must start after cache deletion, survive a missing export drive, and preserve edits across an update. Offer catalog backup/restore and support diagnostics that exclude photographs by default. Telemetry or cloud synchronization is outside the required offline product.

## 9. Double-click Windows delivery

### Build and installation

Use CMake and a pinned dependency manifest/lockfile. Build the GUI as a Windows application (`WIN32_EXECUTABLE`) so launching `Lucent.exe` does not create a console window. Native workers also run without flashing terminals. A debug/diagnostic build may have developer logging, but it is not the photographer's launch path. [CMake property](https://cmake.org/cmake/help/latest/prop_tgt/WIN32_EXECUTABLE.html)

Produce **`Lucent-Setup-<version>-x64.exe`** with Inno Setup. The graphical installer installs Lucent, offers Start Menu/Desktop shortcuts and optional RAW file associations, and provides uninstall support. Configure the per-user installer with `PrivilegesRequired=lowest`, installing under `%LOCALAPPDATA%\Programs\Lucent` in the initiating user's context. Elevate only the official runtime-prerequisite installer when needed; handle its success, failure, and reboot-required outcomes before offering launch. This avoids installing into a different administrator's profile. Normal Lucent execution uses standard user privileges. [Inno Setup privilege settings](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm)

The installed application is a folder of required binaries/resources with one clear user-facing launcher. “Double-click EXE” does not require pretending that Qt, models, and imaging libraries fit into a dependency-free single binary. An optional portable archive is a later separately tested distribution, not the initial support target.

Bundle all dependencies required for advertised offline features: Qt libraries/platform and image plugins, QML imports, the engine/worker, LibRaw and its enabled codec dependencies, selected OpenCV modules, Little CMS, Lensfun assets, output encoders, inference runtime and redistributable model assets. Use Qt's deployment tools and inspect the result; they do not automatically solve every third-party dependency. [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html) · [CMake/QML deployment](https://doc.qt.io/qt-6/qt-generate-deploy-qml-app-script.html)

Bundle the official offline Microsoft Visual C++ Redistributable installer as a prerequisite where licensing and the chosen toolchain require it. Detect a sufficient installed version; otherwise install it through the graphical setup workflow. Do not ask the photographer to download developer tools or copy arbitrary DLLs from a developer machine. Use documented Microsoft redistribution terms and the official runtime package. [Microsoft redistribution guidance](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170)

Sign the installer and application/worker binaries using Authenticode, with timestamping. Keep signing keys outside the repository. Signing identifies the publisher; it does not guarantee the absence of SmartScreen reputation prompts. Installer/update manifests include version, architecture, hashes, and asset inventory. [Microsoft SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool) · [Inno Setup signing](https://jrsoftware.org/ishelp/topic_setup_signtool.htm)

### CPU first; optional GPU

Every required feature must have a tested local CPU path. GPU selection occurs through an inference/compute-provider abstraction with health checks and a recorded provider. If an optional provider fails or exceeds memory, retry once on the CPU and report it in diagnostics.

Do not build the product's long-term architecture around DirectML alone: ONNX Runtime currently describes its DirectML provider as being in sustained engineering. Evaluate newer Windows ML/provider options later without changing recipe semantics. A provider that needs an online first-run download cannot be required for the offline baseline. [ONNX Runtime DirectML status](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html) · [Windows ML distribution](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/distributing-your-app)

### License-aware packaging

Use Qt modules eligible for the chosen LGPL/commercial route, normally dynamically linked for the LGPL route. Dynamic linking alone is not the whole obligation: maintain required notices, corresponding source or a compliant source provision for covered libraries/modifications, replacement/relinking rights as applicable, and an accurate inventory of modules and third-party terms. A commercial Qt license is an alternative. [Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations) · [Qt module licensing](https://doc.qt.io/qt-6/licensing.html)

Preserve the separate library, tool, database, dataset, and weight terms in Appendix C. Inno Setup currently requests commercial license purchases in its stated circumstances but says purchase is not strictly required; review the terms at release instead of treating this as either a compulsory fee or permission to ignore them. [Inno Setup commercial terms](https://jrsoftware.org/isorder.php)

Maintain an SBOM and shipped-license bundle from the actual build. Proprietary application distribution can coexist with compatible library terms when obligations are met; source availability alone does not establish that every research weight or dataset may be redistributed. Nothing in this plan requires a subscription for the photographer.

## 10. Validation, release gates, and roadmap

### Qualification corpus

Obtain legally usable fixtures from the target camera/mode matrix. Include daylight, sunset, blue hour, foliage, water, mountains, fog, snow, night skies, high ISO, backlighting, strongly saturated subjects, underexposure, and deliberately dark/bright compositions. Include different lenses, orientations, resolutions, compression settings, corrupted/truncated files, missing profiles, and files with multiple embedded images.

Split learned-policy development and evaluation by **camera model and trip/scene**, keeping near-duplicates and brackets together. Include held-out camera models so generalization is tested rather than assumed. A decoder support list cannot substitute for these images.

### Required release gates

| Gate | Passing evidence |
|---|---|
| Camera correctness | Each advertised verified model/mode decodes with correct orientation, dimensions, CFA/linear route, black/white handling, required corrections, and color interpretation |
| Natural automatic results | Blind preference and correction-effort evaluation against fixed rendering, a shared preset, LUT-only enhancement, and the proposed optimizer; report defects and confidence by camera/scene |
| Learned-method promotion | Improves a prespecified held-out quality criterion without exceeding a prespecified defect budget; otherwise remain optional or disabled |
| Numerical color/model behavior | Tested transforms, transfer functions, identity LUTs, ramps, cube corners, interpolation, gamut handling, CPU/native-reference parity, and no NaN/Inf |
| Preview/detail fidelity | Shared frozen recipe, stale-result suppression, correct 100% detail, stable masks, and documented preview/export tolerances |
| Original preservation | Source hashes unchanged after import, edit, batch export, cancel, restart, and update |
| Batch resilience | A 1,000-file mixed corpus completes without unbounded memory growth; injected corrupt files, worker crashes, disk-full, cancellation, and drive removal do not lose edits or falsely report completion |
| Restart/publication | Termination injected before/after image rename, sidecar write, and database commit recovers without duplicate success records or unnoticed partial output |
| Offline installation | Clean supported Windows machine with no Python, Qt SDK, Git, compiler, or development PATH installs and imports/edits/exports entirely through the GUI while offline |
| Update compatibility | Prior catalog/recipe fixtures migrate safely; unknown newer operations are reported; old missing assets are not silently replaced |
| Distribution | Exact shipped dependencies, models, profiles, and notices pass the chosen licensing and packaging review |

Set numerical image-quality thresholds using calibration data before final qualification; do not invent passed PSNR, Delta E, speed, or user-preference results. Compare actual images at 100% and at realistic viewing sizes. Document a permissible crop/geometry difference before comparing renderers.

### Performance targets to measure

Target a responsive UI during processing, immediate cached thumbnails, and sub-second proxy feedback after common adjustments when practical. Measure cold-start and warm-start time, time to first RAW preview, median/p95 per-image analysis/export time, peak RAM, CPU utilization, cancellation latency, and completed images per minute on a documented 16 GB laptop. These are engineering targets, not demonstrated figures.

The first optimization is reducing unnecessary work: decode reuse, cached analysis, bounded candidates, final rendering only for the selected recipe, and memory-aware concurrency. Introduce GPU acceleration or libvips only when a measured bottleneck and quality-preserving integration justify them.

### Implementation milestones

1. **Reference and rights baseline.** Pin research snapshots; reproduce the selected methods on supplied examples; record model/data permissions and numerical outputs. Acquire real multi-camera fixtures. Choose tested stable production library releases.
2. **Installable vertical slice.** Double-click installer and GUI; input/output folder selection; LibRaw adapter; one correctly color-managed RAW-to-JPEG/TIFF path; catalog, metadata, and error handling. Demonstrate offline operation on a clean machine early.
3. **Shared editor and recipes.** Detailed controls, masks, undo/redo, preview refinement, immutable recipes, color/geometry contracts, and replay tests. Establish Bayer and non-Bayer route boundaries.
4. **Automatic batching.** Deterministic per-image controller, memory admission, durable queue, checked publication, cancellation/resume, confidence flags, and reports. Complete the 1,000-file fault-injection run.
5. **Multi-brand qualification.** Pass the planned Canon, Sony, DJI, and conventional Insta360 fixtures; publish an exact model/mode support table. Expand Nikon/Fujifilm/other fixtures without changing the UI workflow.
6. **Learned appearance qualification.** Modernize/reproduce Image-Adaptive-3DLUT; establish native inference parity and model domain controls; promote only after comparison with the deterministic baseline. Exposure-based learned proposals remain a separate experiment.
7. **Release hardening.** Signed offline installer, update/migration rehearsal, clean-machine tests, license inventory, performance report, documentation, and support diagnostics.
8. **Later expansions.** Additional cameras/codecs and operating systems, personalized models, camera-qualified RAW denoising, projection-aware panorama editing, native 360 stitching, HDR, and burst workflows as separate tested modules.

Do not postpone basic multi-camera correctness or a working installer until after a sophisticated learned editor. The app is ready when the complete photographer workflow works reliably, not when a research demo produces one attractive image.

## Appendix A. Algorithm and paper evidence

The following research notes are carried forward from the 20 September 2026 blueprint, with the ranking language adjusted for Lucent. Camera-specific references to R5 are examples from that research stage; the same reproduction and validation requirements now apply across the qualification matrix. The production architecture and compatibility contracts above supersede the earlier Python-first application assumptions.

### Exposure — automatic selection of editable operations

**Paper:** Yuanming Hu, Hao He, Chenxi Xu, Baoyuan Wang and Stephen Lin, *Exposure: A White-Box Photo Post-Processing Framework*, ACM TOG, presented at SIGGRAPH 2018. [Author paper](https://arxiv.org/abs/1709.09602) · [Official code](https://github.com/yuanming-hu/exposure) · [MIT source license](https://github.com/yuanming-hu/exposure/blob/master/LICENSE)

The method learns a sequence of parameterized photographic operations using differentiable filters and reinforcement learning. It is directly relevant to choosing settings that a photographer can inspect and change. Use it as the research reference for a parameter-proposal model; the initial bounded optimizer can operate with deterministic proposals while a compatible learned policy is being reproduced.

Reproduce `evaluate.py` on the provided sample TIFFs and inspect `filters.py` for operation definitions. The historical command is `python3 evaluate.py example pretrained models/sample_inputs/*.tif`; the repository uses a pretrained-model submodule. The input pipeline is based on linearized ProPhoto RGB and must not be confused with the selected LUT model's encoded sRGB input. TensorFlow 1-era APIs require modernization. Upstream inference can vary because of dropout: application inference must use a documented fixed sampling policy, store the seed and freeze the selected recipe for reproducibility.

The author's README describes a proof of concept requiring product engineering. No R5-ready checkpoint, batch scheduler, or universally optimal setting predictor is established by this code. Check model/data rights separately, and train or adapt proposals to the application's actual renderer before enabling them by default. A numeric citation count for this newly added paper was not verified.

### 1. Image-adaptive 3D LUTs — selected learned appearance research

**Paper:** Hui Zeng, Jianrui Cai, Lida Li, Zisheng Cao and Lei Zhang, *Learning Image-adaptive 3D Lookup Tables for High Performance Photo Enhancement in Real-time*, IEEE TPAMI 44(4), 2058–2073, 2022; preprint and online publication in 2020. [Author preprint](https://arxiv.org/abs/2009.14468) · [Repository](https://github.com/HuiZeng/Image-Adaptive-3DLUT) · [Apache-2.0 license](https://github.com/HuiZeng/Image-Adaptive-3DLUT/blob/master/LICENSE)

- **Mechanism:** predict weights for basis LUTs from a downsampled image, combine them, and apply the resulting LUT using trilinear interpolation.
- **Why selected:** it separates image understanding from the expensive full-resolution pass. That is attractive for R5 files and a CPU-first application. This is an architectural inference, not a measured laptop benchmark.
- **Input/output:** select the repository's sRGB model for a controlled, rendered sRGB-to-sRGB enhancement experiment. It also contains a distinct XYZ path. Neither should receive a CR3 file or unconverted Bayer/camera RGB data.
- **Reproduce:** start with `demo_eval.py`, the matching `pretrained_models/sRGB` files and bundled sRGB examples. Follow the repository's documented extension build and `models_x` changes when using its newer PyTorch path.
- **Integration work:** modernize the obsolete dependencies; compare a CPU trilinear sampler against the reference; verify tensor layout, interpolation, checkpoint loading and preprocessing. Evaluate inference on a fixed analysis image and reuse its LUT for preview and export.
- **Limits:** identical input colors receive identical output colors regardless of location. A mountain and sky region of the same color cannot receive different adjustments from this LUT alone. It neither denoises RAW nor recovers clipped sensor information. Natural appearance depends on training and user preference.
- **Readiness:** recommended research starting point, not a turnkey SDK. The inspected repository head dates to November 2022. Verify checkpoint and training-data permissions independently before distribution.

### 2. Guided Image Filtering — selected local-adjustment primitive

**Paper:** Kaiming He, Jian Sun and Xiaoou Tang, *Guided Image Filtering*, ECCV 2010; extended TPAMI publication. [Author project and paper](https://people.csail.mit.edu/kaiming/eccv10/index.html) · [OpenCV implementation](https://github.com/opencv/opencv_contrib/blob/4.x/modules/ximgproc/src/guided_filter.cpp)

The guided filter models local output as a linear function of a guidance image. Its efficient formulation is useful for edge-aware smoothing and mask refinement. Use a documented luminance representation to derive an adjustment field, then apply bounded exposure changes in the linear working image. Avoid assuming that filtering itself chooses aesthetically correct edits.

Reproduce through `cv2.ximgproc.guidedFilter` from `opencv-contrib-python` on float32 test images, then compare with the paper's examples. Record radius, regularization epsilon, input scale and guidance channels. Epsilon depends on the numeric range; settings for 8-bit inputs do not transfer unchanged to normalized floats. Test mountain/sky boundaries, thin branches and low-contrast clouds for halos or lost contrast.

The inspected OpenCV source file retains a BSD-3-Clause header; the broader current OpenCV-contrib package uses Apache-2.0 with component notices. Do not substitute a mirrored copy of the author's academic-only MATLAB demo without checking its terms. [Package license](https://github.com/opencv/opencv_contrib/blob/4.x/LICENSE)

### 3. Malvar–He–Cutler demosaicing — selected replication baseline

**Paper:** Henrique Malvar, Li-wei He and Ross Cutler, *High-quality linear interpolation for demosaicing of Bayer-patterned color images*, ICASSP 2004. [Microsoft Research publication](https://www.microsoft.com/en-us/research/publication/high-quality-linear-interpolation-for-demosaicing-of-bayer-patterned-color-images/) · [BSD-3-Clause implementation](https://github.com/colour-science/colour-demosaicing/blob/develop/colour_demosaicing/bayer/demosaicing/malvar2004.py)

This method uses small linear filters to reconstruct missing Bayer colors. Its compact mathematical structure makes it an excellent first paper to replicate and a useful correctness baseline. Call `demosaicing_CFA_Bayer_Malvar2004` on a normalized, single-channel mosaic with the actual visible-image CFA pattern. The output is camera RGB unless a separate color transform is applied.

Test all four Bayer phases, image borders and shifted crops. Never infer phase solely from an assumed camera model. The NumPy/SciPy implementation is a reference: full-frame intermediates can be expensive at 45 MP. A native small-kernel implementation can be considered after validating equivalence. This is not selected as the final quality winner.

Use the [colour-demosaicing license](https://github.com/colour-science/colour-demosaicing/blob/develop/LICENSE) for that implementation. The separate [IPOL MHC package](https://www.ipol.im/pub/art/2011/g_mhcd/) documents mixed permissions and possible patent concerns; do not describe its entire archive as permissively licensed.

### 4. AMaZE and RCD — landscape quality references

**Algorithms and code:** [RawTherapee AMaZE](https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/amaze_demosaic_RT.cc), [RawTherapee RCD](https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/rcd_demosaic.cc), and [RCD author's explanation/code](https://github.com/LuisSR/RCD-Demosaicing).

Use these to compare foliage, rock texture, fine branches, stars and false color. [RawTherapee guidance](https://rawpedia.rawtherapee.com/Demosaicing) favors AMaZE for fine detail at lower noise levels and discusses RCD and noisier-image alternatives. Treat that as project guidance, not an R5 test result.

No original peer-reviewed AMaZE or RCD paper was verified in this research. Label these engineering algorithms accurately rather than inventing academic citations. The maintained RawTherapee implementations are GPL-3-or-later in their headers and depend on application internals. They are comparison references for the current commercial-flexibility path. Direct reuse requires choosing a compatible distribution strategy. Stock rawpy wheels do not include the optional GPL demosaic packs. [rawpy build features](https://github.com/letmaik/rawpy/blob/main/README.md)

### 5. Local Laplacian Filters — alternative local-tone engine

**Paper:** Sylvain Paris, Samuel Hasinoff and Jan Kautz, *Local Laplacian Filters: Edge-aware Image Processing with a Laplacian Pyramid*, ACM TOG/SIGGRAPH 2011. [Author paper and materials](https://people.csail.mit.edu/sparis/publi/2011/siggraph/) · [Halide implementation](https://github.com/halide/Halide/tree/main/apps/local_laplacian)

This is a stronger research option for multiscale detail and tonal adjustment. The method manipulates local detail while treating large edges differently. Compare its naturalness with the simpler guided-filter adjustment, particularly at horizon boundaries.

Reproduce the Halide app using its supplied build instructions and example inputs, then tune the exposed parameters on held-out landscape crops. Halide is a useful performance implementation reference, but its app is not automatically identical to every setting of the original method. Record the approximation and schedule used. Review the [Halide license and component notices](https://github.com/halide/Halide/blob/main/LICENSE.txt); the main project is MIT licensed.

### 6. Fast Fourier Color Constancy — later automatic-white-balance research

**Paper:** Jonathan Barron and Yun-Ta Tsai, *Fast Fourier Color Constancy*, CVPR 2017. [Publication](https://research.google/pubs/fast-fourier-color-constancy/) · [Official MATLAB repository](https://github.com/google/ffcc) · [Apache-2.0 license](https://github.com/google/ffcc/blob/master/LICENSE)

FFCC estimates illumination using log-chrominance statistics and frequency-domain computation. The official code provides training, cross-validation and evaluation, with a `minFunc` dependency. Its input is a black-level-corrected linear thumbnail consistent with the training pipeline, not a final gamma-encoded JPEG.

Start by reproducing the supplied dataset setup, then create an R5-specific calibration/evaluation set. Camera white balance metadata is the first app's default. Automatic neutralization can erase intentional golden-hour warmth or blue-hour mood; an FFCC estimate should be an optional suggestion. The official repository is archived. Treat its reported speed as paper-specific, not a forecast for this application.

### 7. HDRNet — alternative to the selected LUT approach

**Paper:** Michael Gharbi, Jiawen Chen, Jonathan Barron, Samuel Hasinoff and Frédo Durand, *Deep Bilateral Learning for Real-Time Image Enhancement*, SIGGRAPH 2017. [Paper](https://arxiv.org/abs/1707.02880) · [Official repository](https://github.com/google/hdrnet) · [Apache-2.0 license](https://github.com/google/hdrnet/blob/master/LICENSE)

HDRNet predicts a bilateral grid of local transformations from a lower-resolution input, then applies them with image guidance. Its spatial adaptation is attractive when a global LUT cannot express the desired edit.

It is the comparison architecture, not the initial selection: the official repository is archived, its documented environment is old, and the documented BilateralSliceApply operator is GPU-only. Reproduction starts with the supplied operator tests and a checkpoint matched to the exact input pipeline. In particular, its HDR+ checkpoint expects specially prepared linear data; the README explicitly warns against feeding it ordinary sRGB. A CPU-capable rewrite needs numerical parity tests, not merely an API conversion.

### 8. Unprocessing Images — preferred foundation for later RAW denoising

**Paper:** Tim Brooks, Ben Mildenhall, Tianfan Xue, Jiawen Chen, Dillon Sharlet and Jonathan Barron, *Unprocessing Images for Learned Raw Denoising*, CVPR 2019. [Paper](https://openaccess.thecvf.com/content_CVPR_2019/html/Brooks_Unprocessing_Images_for_Learned_Raw_Denoising_CVPR_2019_paper.html) · [Official Google Research code](https://github.com/google-research/google-research/tree/master/unprocessing)

The method approximately reverses an RGB imaging pipeline to create synthetic RAW-like training examples with signal-dependent noise. It is useful as a training strategy; it does not recover genuine original sensor measurements from a JPEG.

Reproduce the documented denoising setup on its original benchmark first. For R5 adaptation, replace assumed camera matrices, gain distributions and noise statistics with appropriate measured values. Preserve the model's Bayer packing, signal scale and noise-variance contract. The inspected source uses Apache-2.0, while datasets and externally hosted models need their own review. Keep neural denoising optional until it improves noise without removing foliage or inventing convincing texture.

### 9. Learning to See in the Dark — influential RAW-to-RGB reference

**Paper:** Chen Chen, Qifeng Chen, Jia Xu and Vladlen Koltun, *Learning to See in the Dark*, CVPR 2018. [Paper](https://openaccess.thecvf.com/content_cvpr_2018/html/Chen_Learning_to_See_CVPR_2018_paper.html) · [Official MIT-licensed repository](https://github.com/cchen156/Learning-to-See-in-the-Dark)

SID is an important demonstration of learning low-light rendering directly from RAW. Reproduce a provided Sony or Fuji experiment with its matching dataset and checkpoint before adapting it.

It is not my first app choice: the repository explicitly cautions that pretrained models probably will not work for a different camera sensor. Its original environment is Python 2.7/TensorFlow 1, and original training eagerly loads large datasets. Never copy a Sony black-level constant into R5 preprocessing. Replace eager loading with patch-based, disk-backed sampling if pursuing retraining. Code licensing does not establish dataset redistribution rights.

### 10. BM3D — important classical benchmark, restricted official implementation

**Paper:** Kostadin Dabov, Alessandro Foi, Vladimir Katkovnik and Karen Egiazarian, *Image Denoising by Sparse 3-D Transform-Domain Collaborative Filtering*, IEEE TIP 2007. [Authors' paper/software page](https://webpages.tuni.fi/foi/GCF-BM3D/index.html) · [Official terms](https://webpages.tuni.fi/foi/GCF-BM3D/legal_notice.html)

BM3D groups similar patches and denoises them jointly in a transform domain. It is an influential benchmark for detail/noise tradeoffs. RAW's signal-dependent noise needs appropriate treatment; a generic additive-Gaussian-noise example is not a full sensor pipeline.

The official implementation has noncommercial and redistribution restrictions. It is not selected as a commercial app dependency, and installing a Python wrapper does not remove the underlying terms. No independently audited permissive GitHub implementation is endorsed here. Reproduce only within the actual implementation's permitted use.

### 11. HDR+ — later repeated-burst extension

**Paper:** Samuel Hasinoff and colleagues, *Burst Photography for High Dynamic Range and Low-Light Imaging on Mobile Cameras*, ACM TOG/SIGGRAPH Asia 2016. [Original paper](https://graphics.stanford.edu/papers/hdrp/hasinoff-hdrplus-sigasia16-preprint.pdf) · [C++/Halide reproduction](https://github.com/timothybrooks/hdr-plus) · [IPOL independent analysis](https://www.ipol.im/pub/art/2021/336/) · [IPOL Python implementation](https://github.com/amonod/hdrplus-python)

HDR+ aligns and robustly merges roughly same-exposure RAW bursts before rendering. It is valuable for handheld dusk scenes, but it adds capture, registration, motion and memory complexity beyond the first version, which batch-processes independent photographs without merging them.

Use the MIT-licensed C++/Halide repository as a performance-oriented candidate and the AGPL-3.0-or-later IPOL implementation as a scientific reference. Neither is Google's production source. Reproduce published DNG examples before adding CR3 ingestion. The IPOL README flags a possible patent; record this upstream notice without asserting its applicability. Test moving leaves, water, parallax and failed alignment with a single-frame fallback.

### 12. Mertens Exposure Fusion — later bracketed-exposure extension

**Paper:** Tom Mertens, Jan Kautz and Frank Van Reeth, *Exposure Fusion*, Pacific Graphics 2007. [Author paper](https://jankautz.com/publications/exposure_fusion.pdf) · [OpenCV implementation](https://github.com/opencv/opencv/blob/4.x/modules/photo/src/merge.cpp) · [API documentation](https://docs.opencv.org/4.11.0/d6/df5/group__photo__hdr.html)

Exposure fusion combines contrast, saturation and well-exposedness weights across a multiscale pyramid. It offers a straightforward later feature for tripod brackets. Its output is a fused rendered image, not a scene-linear radiance master.

Develop brackets consistently, align them, then reproduce `createMergeMertens().process(...)` with the documented input convention. The inspected implementation scales input by 1/255 internally; normalized float inputs cannot be substituted casually. Alignment and moving-object deghosting are separate responsibilities. OpenCV uses Apache-2.0 with component notices. Keep this workflow distinct from HDR+ and a true RAW radiance merger.

## Appendix B. Academic influence — historical citation snapshot

The following counts were retrieved from the OpenAlex API on 20 September 2026. They are counts for those specific indexed records, not universal totals. Publication versions may be split; no duplicate counts have been added. Technical selection still depends on reproducibility and the actual application.

| Paper/record | Indexed citations | Evidence |
|---|---:|---|
| Malvar–He–Cutler, ICASSP 2004 | 441 | [OpenAlex W2151502878](https://api.openalex.org/works/W2151502878) |
| Guided Image Filtering, ECCV 2010 | 1,587 | [OpenAlex W2160956336](https://api.openalex.org/works/W2160956336) |
| Local Laplacian Filters, TOG 2011 | 344 | [OpenAlex W1993465480](https://api.openalex.org/works/W1993465480) |
| HDRNet, 2017 | 837 | [OpenAlex W2735974062](https://api.openalex.org/works/W2735974062) |
| FFCC, 2017 | 213 | [OpenAlex W2556952061](https://api.openalex.org/works/W2556952061) |
| BM3D, IEEE TIP 2007 | 9,339 | [OpenAlex W2056370875](https://api.openalex.org/works/W2056370875) |
| Learning to See in the Dark, 2018 | 1,399 | [OpenAlex W2799265886](https://api.openalex.org/works/W2799265886) |
| HDR+, 2016 | 500 | [OpenAlex W2552290192](https://api.openalex.org/works/W2552290192) |

For image-adaptive LUTs, later primary papers explicitly build on the method, including [AdaInt, CVPR 2022](https://openaccess.thecvf.com/content/CVPR2022/papers/Yang_AdaInt_Learning_Adaptive_Intervals_for_3D_Lookup_Tables_on_Real-Time_CVPR_2022_paper.pdf). A numeric citation total was not verified for the LUT paper. Its selection is based on the application fit, not an unsupported “most cited” claim. Unprocessing is retained for technical merit without a numeric influence claim.

## Appendix C. Repository readiness and licensing

These are inspected project terms and integration observations, not a blanket conclusion about every distribution scenario. Copyright licenses, patent issues, training-data permission and model-weight permission must not be conflated.

| Repository/component | Inspected terms | Decision |
|---|---|---|
| [LibRaw](https://github.com/LibRaw/LibRaw/blob/master/COPYRIGHT) | Choice of LGPL-2.1 or CDDL-1.0; optional components can add terms | Selected; record build flags and chosen compliance path |
| [rawpy](https://github.com/letmaik/rawpy/blob/main/LICENSE) | MIT wrapper; underlying LibRaw obligations remain | Selected for prototyping; record bundled LibRaw version |
| [Image-Adaptive-3DLUT](https://github.com/HuiZeng/Image-Adaptive-3DLUT/blob/master/LICENSE) | Apache-2.0 source | Selected research foundation; separately resolve model/data use |
| [Exposure](https://github.com/yuanming-hu/exposure/blob/master/LICENSE) | MIT source; legacy TensorFlow implementation | Explicit-edit proposal reference; model submodule and data permissions assessed separately |
| [OpenCV-contrib](https://github.com/opencv/opencv_contrib/blob/4.x/LICENSE) | Apache-2.0 project; guided-filter source retains BSD-3-Clause | Selected; preserve relevant component notices |
| [colour-demosaicing](https://github.com/colour-science/colour-demosaicing/blob/develop/LICENSE) | BSD-3-Clause | Selected educational/numerical reference |
| [Lensfun](https://github.com/lensfun/lensfun/blob/master/README.md) | LGPL-3 library; GPL-3 tools; CC BY-SA-3.0 database | Selected with separate library/database obligations |
| [Little CMS](https://github.com/mm2/Little-CMS/blob/master/LICENSE) | MIT | Selected color-management engine |
| [libvips](https://github.com/libvips/libvips/blob/master/README.md) | LGPL-2.1-or-later | Later infrastructure option, not essential to first replication |
| [RawTherapee](https://github.com/RawTherapee/RawTherapee/blob/dev/LICENSE), [darktable](https://github.com/darktable-org/darktable/blob/master/LICENSE), [ART](https://github.com/artraweditor/ART/blob/master/LICENSE.txt) | GPL-family application code | Rendering/workflow references; direct embedding needs a compatible product decision |
| [Google FFCC](https://github.com/google/ffcc), [Google HDRNet](https://github.com/google/hdrnet) | Apache-2.0; archived repositories | Research alternatives requiring modernization |
| [SID](https://github.com/cchen156/Learning-to-See-in-the-Dark/blob/master/LICENSE.md) | MIT source | Cross-camera research reference, not R5-ready weights |

LibRaw and rawpy are libraries; RawTherapee, darktable and ART are full applications. Their internal image-processing code is not a stable drop-in SDK. A command-line boundary alone should not be described as an automatic solution to all licensing obligations.

### Source snapshots inspected on 20 September 2026

These are inspected development heads, not a claim that they are the best release builds to ship. Use the LUT snapshot for replication; for production libraries, select and test stable releases before distribution.

| Repository | Inspected commit | Commit date |
|---|---|---|
| Image-Adaptive-3DLUT | `b491f6df64a588864739a157db271e5c848e1805` | 2022-11-26 |
| Exposure | `7bb838a5f977801ed30baa194edce2b574fd4b29` | 2019-03-16 |
| rawpy | `a39c2e7a44911889c3360891012f862f904ba551` | 2026-08-30 |
| LibRaw | `116924e5bcb9483ef9c5fbe7f3cfc5a8a4b97064` | 2026-09-13 |
| OpenCV-contrib | `a8e9acd62cabd30419dba83007f2ac0d07de5e2c` | 2026-07-14 |

## Appendix D. Reproduction and implementation handoff

These steps are for Lucent's developers. The installed application does not expose them to the photographer or require their environments.

### D1. Establish a decoder and color reference

Start in a pinned Python/rawpy research environment to inspect each fixture's format, dimensions, raw pattern, black/white levels, white balance, and metadata. Record `rawpy.__version__`, `rawpy.libraw_version`, and `rawpy.flags`, and retain input hashes. Use the [official rawpy examples](https://github.com/letmaik/rawpy/blob/main/README.md) and [API](https://letmaik.github.io/rawpy/api/rawpy.RawPy.html).

An initial rendered diagnostic can use as-shot white balance, disabled auto-brightening, AHD for a supported Bayer sample, and 16-bit output. Explicitly record output color space and transfer function. A rendered sRGB array is not an unclipped scene-linear master. Test Canon RAW/C-RAW and each other camera's relevant modes independently.

Then implement the native LibRaw adapter and compare metadata and numeric results with the reference on the same decoder build. Port no Python assumptions about a fixed CFA, two interchangeable greens, a single white level, one subimage, or one camera matrix into the generic interface. Select tested stable library releases for distribution rather than blindly shipping the inspected development heads.

### D2. Reproduce Image-Adaptive-3DLUT

1. Check out the Appendix C snapshot; inspect `demo_eval.py`, `models.py`, `models_x.py`, the extension implementation, and the matching pretrained-model directory.
2. Reproduce its supplied sRGB example in an isolated research environment before modernizing. The historical command is `python demo_eval.py --input_color_space sRGB`; old dependencies and extension builds require adaptation. This command was not executed for this blueprint.
3. Save example inputs, checkpoints, and output-array hashes. Confirm that the checkpoint actually loads and runs; an accessible repository is not successful replication.
4. Preserve resizing, channel and LUT-axis order, interpolation boundaries, and model outputs. Do not add a softmax to mixture coefficients simply because they are called weights; that changes the reference method.
5. Validate native float32 trilinear LUT application using identity cubes, neutral/channel ramps, cube corners, and interpolation midpoints. Compare full-frame and tiled results.
6. Export or reimplement the small predictor for ONNX Runtime CPU only after reference outputs exist. Treat successful export, operator support, and numeric parity as explicit gates. No claim is made here that the historical model already exports cleanly.
7. Freeze a LUT from one fixed analysis image and reuse it for previews/tiles/export. Recompute when its upstream model input changes. Test intended camera domains and sRGB appearance separately from a wide-gamut route.

### D3. Reproduce explicit-edit and local-filter references

Reproduce [Exposure](https://github.com/yuanming-hu/exposure) independently on its supplied TIFFs. Inspect `filters.py`, `evaluate.py`, the pretrained-model submodule, and its linearized ProPhoto convention. Its historical example is `python3 evaluate.py example pretrained models/sample_inputs/*.tif`. Modernizing its TensorFlow 1 environment is research work; it is not Lucent's launch command. Fix stochastic sampling/dropout behavior for evaluation, record seeds, and do not assume a paper filter parameter equals an editor slider.

Reproduce the [OpenCV guided filter](https://github.com/opencv/opencv_contrib/blob/4.x/modules/ximgproc/src/guided_filter.cpp) independently on ramps, edges, and photographs. Build and validate Lucent's bounded local-luminance and chroma-noise operations around it. Check scale, halos, masks, and composed tile overlap. Compare Local Laplacian as an alternative only after this baseline works.

Use [colour-demosaicing](https://github.com/colour-science/colour-demosaicing) for an MHC numerical reference and RawTherapee for visual AMaZE/RCD comparisons. Different input profiles or tone curves can dominate such comparisons; hold the color/rendering choices constant or document what differs.

### D4. Validate policy, data, and distribution

Build the deterministic optimizer first, then compare fixed baseline, shared preset, LUT-only, deterministic optimization, and optional learned proposals on the same corpus. Measure technical defects, photographer preference, manual correction effort, time, and memory. Freeze metric normalization, bounds, confidence thresholds, and style-policy versions only after calibration.

The [MIT–Adobe FiveK project](https://people.csail.mit.edu/vladb/photoadjust/) is a useful photographic-adjustment research reference. The previous research did not establish permission to commercially train on or redistribute its images or derived weights; do not assume that permission from a code license. Use photographs/targets with documented rights and obtain appropriate dataset/model terms. Split by camera and trip/scene, not random nearly identical frames. The adequate training-set size is an empirical question, not a promise that a few hundred R5 photos will generalize to all cameras.

Create a dependency ledger for the native release: Qt modules, LibRaw codecs/build flags, selected OpenCV modules, Lensfun library/database, Little CMS, SQLite, output encoders, ONNX Runtime, models, and the Windows runtime. Use upstream repositories and stable release artifacts; record resolved versions, hashes, patches, and notices. Useful additional upstream repositories are [Qt Quick](https://github.com/qt/qtdeclarative), [Qt Base](https://github.com/qt/qtbase), [ONNX Runtime](https://github.com/microsoft/onnxruntime), [CMake](https://github.com/Kitware/CMake), and [Inno Setup](https://github.com/jrsoftware/issrc). These supply infrastructure; they do not implement Lucent's photographic policy.

## Appendix E. Later research worth tracking

This retained shortlist is dated 20 September 2026. These methods do not replace the first release's qualified, camera-independent baseline.

| Method | Paper and implementation | Integration boundary |
|---|---|---|
| ELD: Physics-Based Noise Formation, CVPR 2020 | [Paper](https://arxiv.org/abs/2003.12751), [official code](https://github.com/Vandermode/ELD) | MIT source exists, but the README withholds core noise-model/calibration material for business licensing; source visibility does not establish full reproduction |
| PMN: Learnability Enhancement for Low-Light RAW Denoising, ACM MM 2022 / TPAMI extension | [Paper](https://arxiv.org/abs/2207.06103), [official code](https://github.com/megvii-research/PMN) | Useful calibration/noise ideas; existing calibration is not proof of cross-camera support; assess source, external resources, and weights separately |
| RawNIND joint denoising/demosaicing, 2025 | [Paper](https://arxiv.org/abs/2501.08924), [author code](https://github.com/trougnouf/rawnind_jddc), [darktable model package](https://github.com/darktable-org/darktable-ai/blob/master/models/rawdenoise-nind/README.md) | Emerging deployment reference with separate code/weight terms; camera and accelerator behavior require independent tests |

## Appendix F. Document provenance

This revision was prepared from the user-supplied `canon-r5-raw-processing-blueprint.md`, revision 2 dated 20 September 2026. Its SHA-256 was `63A5E380C8156C2B9AC124643DCDACFE30AAFE8590A7883E7F03E271954D5F84`. The original file was retained unchanged.

Research citations support specific component capabilities, papers, and terms. The architecture, initial tuning ranges, release gates, and milestones are proposed design decisions. Paper citation counts remain dated snapshots, and publication benchmarks are not Lucent performance measurements. This document makes no claim that the app, installer, camera tests, or model replication have already been completed.
