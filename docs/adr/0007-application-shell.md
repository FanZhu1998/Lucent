# ADR 0007: Application shell: UI adapters over in-process application services

- **Status:** Accepted
- **Date:** 2026-09-27 (revised the same day after the W3 review: executor lanes, serial exports,
  embedded-preview selection, import rules, labelling of camera and incomplete renders)
- **Workflow:** W3 (Application shell)

## Context

Blueprint section 4 puts a Qt Quick interface (Library / Edit / Exports) on top of application
services, and those services on top of the engine and the exporter. It requires that "the UI never
decodes RAW on the UI thread", that "each preview request carries image ID, recipe revision,
geometry revision, quality level, and generation token", that "a late result from an earlier
slider position must never replace the current preview", and that interactive work has
"reserved capacity so a background batch cannot make detailed editing unusable". Section 2 asks
for an embedded camera JPEG that "may appear immediately for browsing, labeled until replaced by
Lucent's own render". Section 7 asks for one full-resolution render at a time on the reference
laptop and for exports that never overwrite. Section 3 defines the compatibility states.

W3 produced three pieces in parallel:

- `lucent_application` (src/application): a pure C++ services library with no Qt. A `Library`
  interface with an in-memory `SessionLibrary`; `import_sources()`; a two-lane `TaskExecutor`
  with `MemoryAdmission` (three lanes since the review, decision 7); a `PreviewService` with
  channels, generations and an LRU cache; camera previews from `raw::extract_embedded_preview()`
  (decision 11); `ExportService` over the exporter.
- `Lucent.exe` (apps/lucent): the Qt Quick shell, written against UI-side service interfaces
  (`services/*.hpp`) and first run on simulated services.
- The binding between the two, which this ADR records, together with the contracts the shell
  relies on.

Worker processes (crash containment, release of native allocations) are W7's. The catalog is
W4's. Editing controls and recipes are W6's.

Measured on the reference laptop (Ryzen 5 5500U, 12 GB, 150 % display scale) with Lucent.exe
and the five smoke fixtures, 2026-09-27 (`--measure`; see "Developer automation"). These are
timings of an otherwise idle application. Before the review, the thumbnail time held only while
no export was queued: camera previews shared the Background lane with imports and exports, so
queued exports (which wait for the memory admission on a worker thread) or a long import delayed
every thumbnail behind them. Decision 7 removes that dependency; the adapter tests pin it.

| What | Result |
|---|---|
| Window loaded after process start | 0.60 to 0.91 s |
| First camera-preview thumbnail visible after the import started | 0.13 to 0.29 s; after the review (decisions 7 and 11), 0.12 to 0.14 s, and 0.29 s in a run with a cold disk cache |
| Canon EOS R5 (45 MP) Fit preview visible after it was requested | 1.06 s (service: engine 0.93 s, request to pixels 1.02 s); after the review 1.02 to 1.08 s |
| Canon EOS R5 100 % preview visible after the zoom | 1.31 s (service: engine 0.90 s, request to pixels 1.21 s) |
| UI thread during renders and exports: largest gap between 5 ms timer ticks (window grabs excluded) | 8 to 89 ms per run; at most one gap over 50 ms, none over 100 ms. After the review: 17 to 90 ms, and one 257 ms gap in the cold-cache run |
| JPEG sRGB export (render, encode, write), R5 / A7 IV / X-T4 / Z 6 | 4.0 / 2.9 / 3.5 / 2.3 s |
| 16-bit Display P3 TIFF export, same files | 18.5 / 12.8 / 11.6 / 9.5 s |

## Decision

1. **Layering.** `Lucent.exe` → UI adapters (`apps/lucent/src/adapters`, target
   `lucent_ui_adapters`) → `lucent_application` → `lucent_engine` / `lucent_export` /
   `lucent_raw`. The QML views, view models and controller see only the UI-side interfaces in
   `services/*.hpp`; `service_factory.cpp` binds them to the adapters. Qt stays above
   `lucent_application`; the services never see Qt types and the adapters never implement pixel
   algorithms (conversions are limited to copying 8-bit display pixels, JPEG decoding of camera
   previews and orientation). The simulated services remain as a test double
   (`apps/lucent/tests/fakes`), not part of Lucent.exe.

2. **One Backend per session, owned by the UI thread.** `adapters::Backend` owns, in order, the
   decoder registry, the session library, the executor, the renderer, the preview service and
   the export service, and destroys them in reverse: the services cancel and wait for their
   callbacks, the executor runs every queued task once with a cancelled token and joins its
   threads, and only then are the library and registry released. Work on the executor may
   therefore use the library and registry by reference. No task or callback ever owns the
   Backend; the adapters share it and are destroyed on the UI thread (the QML engine first,
   then the controller that owns the services), so the last reference is always released there.

3. **Threading.** Every application-service callback runs on an executor worker. The adapters
   never touch Qt objects from there: they post to the UI thread through `UiDispatcher`, a queued
   `QMetaObject::invokeMethod` guarded by a mutex that the owner closes at the start of its
   destructor. After closing, no thread can still be posting to the object, and anything posted
   before is discarded by Qt with the object. Library changes are converted to snapshots on the
   committing thread and posted, so `items_added` of an import request always precedes its
   `import_finished`. Nothing is decoded, rendered or encoded on the UI thread: imports and
   exports run on the Background lane (exports one at a time: the export adapter submits a job
   only when the previous one has finished), camera previews on the Quick lane, Edit previews
   and Lucent thumbnails on the Interactive lane, and
   camera-JPEG decoding and 8-bit-to-`QImage` conversion on the image source's own two
   conversion threads. Qt Quick's image-loader thread only queues requests and, as before,
   scales a result down when it exceeds the requested box.

4. **Generations and staleness.** Two levels, both required:
   - *Shell generations.* Every image URL carries item id and generation
     (`image://thumbnail|preview/<item>/<generation>`); the provider rejects a request whose
     generation is not current and discards a result that was superseded while in flight.
     Thumbnail generation 1 is the camera's embedded JPEG, labelled as a camera preview wherever
     it is shown: "Camera preview" on the contact sheet, a "Camera" tag in the filmstrip, and
     the Edit view's placeholder label (blueprint section 2). From generation 2 the thumbnail is
     Lucent's render. A file without a usable camera preview moves to generation 2. After an Edit
     preview is delivered, a thumbnail made from it replaces the camera preview (generation + 1),
     unless the render is incomplete (decision 6). The preview generation is the recipe revision
     (1 until W6) and is passed to the preview service as the settings revision; W6 must fold
     recipe and geometry revisions into it.
   - *Changed files.* When a file changes on disk after import, renders and exports fail with
     SourceChanged ("Add it again to update it"). Importing it again refreshes the item in
     place (`Library::refresh`, `LibraryChangeKind::Refreshed`): the preview service and the
     image source drop their caches for it, and the shell resets its processing state and moves
     both generations on. The thumbnail moves to a new generation of Lucent's render rather than
     back to the camera preview, because Qt Quick may hold generation 1's old pixels under the
     same URL.
   - *Service channels.* All Edit previews share one `PreviewService` channel whose generation
     increases with every request, so a newer request (another photo in the filmstrip, another
     zoom) cancels older renders cooperatively and a late result is reported Superseded, never
     delivered. When the shell cancels the reply of the newest request (the view closed), the
     adapter cancels the channel; cancelling an older reply changes nothing. `ImageReply::
     on_cancel()` carries the shell's cancellation to the source.

5. **Preview quality.** A request box smaller than the photo's full-resolution size is a Fit
   preview with the long edge of the photo fitted into the box (rounded up); the photo's full
   size, a larger box or no box is Full. After the first render the rendered size replaces the
   probe's. The Edit view asks for Fit first and Full at 100 %. Renders of thumbnails for photos
   without a camera preview run one at a time and only while no Edit preview is outstanding.

6. **Support states.** `app::SupportStatus` maps Supported → "Compatible, unverified",
   Limited → Limited, Unsupported → Unsupported. Nothing is Verified before W10's qualification
   corpus. Unsupported files are listed with a specific reason, can be opened (the render or its
   failure explains more), and are refused by export with their reason; they are never exported
   silently. A render that lacks a mandatory interpretation step (`RenderInfo::incomplete`: its
   interpretation maps to Unsupported) is never presented as an accurate result: the Edit view
   shows a banner on the image, the processing state reads "Incomplete render" with an
   explanation, and it does not replace the camera preview as the item's thumbnail (the camera's
   own rendering stays, labelled). The contact sheet shows each photo's processing state
   (Processing, Processed, Failed, Incomplete) as well as its support state.

7. **In-process executor now; worker processes in W7.** Rendering runs in Lucent.exe on the
   application executor, which has three lanes taken in priority order: Interactive (Edit
   previews, Lucent thumbnails), Quick (camera previews: short reads for browsing) and Background
   (imports, exports). Four workers, whatever the core count: one is reserved for interactive
   work and one more for quick work, so the Background lane has two threads. An import therefore
   runs beside an export, and no amount of background work (an export waiting for the memory
   admission, a long import from a card) can hold up browsing or editing (blueprint section 4).
   Exports run one at a time, so a queue of exports occupies one Background thread. One
   full-resolution render is admitted at a time within the engine's default memory budget; the
   worker count does not bound CPU work, admission does. A crash in a decoder ends the
   application in W3. W7 moves rendering and export into `lucent-worker.exe` behind the same
   `SourceRenderer` and service interfaces; the UI adapters and the generation contract do not
   change.

8. **Display colour.** Previews are the engine's display-referred linear Rec.2020 converted to
   8-bit sRGB (`app::encode_display_srgb8`, within one code value of the exporter's sRGB
   conversion) and shown as sRGB without further colour management: Qt Quick and the Windows
   compositor treat them as sRGB. On a wide-gamut display without an sRGB mode they look more
   saturated than they are. Until display-profile support (reading the monitor's ICC profile
   and converting the preview to it), the preview matches an sRGB export on an sRGB display
   only. Camera previews are shown as stored (usually sRGB, sometimes Adobe RGB, which the file
   does not report) and are labelled as camera previews.

9. **User-facing wording.** `lucent::Error::message` is developer-facing and never shown. The
   adapters word errors from the error code and location (`adapters/conversions.hpp`), and
   support reasons from the interpretation issue kind. Per-file import reasons are listed in the
   header's Details popup; export failures appear on the job and in the status line.

10. **Developer automation.** Lucent.exe accepts developer-only options (documented in
    `developer_automation.hpp` and `--help`, not part of the user interface): `--import`,
    `--include-subfolders`, `--open-first`, `--view`, `--export-all`, `--export-format`,
    `--export-color-space`, `--export-quality`, `--zoom`, `--show-export-dialog`, `--measure`,
    `--screenshot`, `--quit-after-screenshot` and `--settle-timeout`. Screenshots wait for idle
    services and two identical rendered frames. `--measure` reports start-up time, the first
    visible thumbnail, preview latency (visible, and the services' render and request-to-pixels
    times) and UI-thread responsiveness as gaps between ticks of a 5 ms timer, with window grabs
    excluded. In a session that presents no frames (headless or locked), layouts only happen
    when a frame is rendered; the automation renders one to lay out the Edit view, and the
    latency clock starts with that frame. Running Lucent.exe from a build tree is described in
    `docs/development/setup.md`.

11. **Embedded camera previews** (`raw::extract_embedded_preview`, `raw/embedded_preview.hpp`).
    The LibRaw adapter identifies the file through the same read-only datastream as probing
    (ADR 0005) and never unpacks the RAW data. Every image in LibRaw's thumbnail list is
    classified (JPEG inside the file, a displayable frame header read directly from the file,
    size limits); formats that are not JPEG are listed and never returned. Selection: without a
    requested size the candidate with the most pixels; with one (the thumbnail's long edge, 512
    px, 768 px at 150 % scale) the smallest candidate whose long edge covers it and whose aspect
    ratio is within 2 % of the largest's (letterboxed thumbnails are never preferred), else the
    largest. The pixels are returned unrotated with the RAW's orientation, the only orientation
    to apply (the JPEG's own EXIF orientation is ignored), and the file identity is verified
    unchanged after reading. The preview service caches previews by item and requested size and
    skips requests cancelled before they start (a card scrolled out of view). On the smoke
    fixtures a 768 px thumbnail reads the 1620 x 1080 preview of the Canon R5 (0.3 MB instead of
    the 3.1 MB full-size one), the 1616 x 1080 of the Sony A7 IV (0.5 MB instead of 7.0 MB) and
    the 1620 x 1080 of the Nikon Z 6; the Fujifilm X-T4 and DJI files have no intermediate
    preview, so the largest is read.

12. **Import rules the shell relies on** (`application/import.hpp`). Explicitly selected files
    (Add photos, drag and drop, command line) are probed whatever their extension, except that a
    file with a non-RAW extension that is not a RAW file (a JPEG or XMP companion) is skipped with
    a reason in the Details, not listed as an Unsupported photo. Output folders chosen for exports
    in the session are skipped only when a scan reaches them below a different import folder
    (the blueprint's nested-output case, or a junction into one); the folder the user selects and
    the files the user selects are never skipped for being in one, so exporting next to the
    originals or into a parent folder never hides them. Exported JPEG and TIFF files are ignored
    by extension in folder scans.

## Consequences

- The shell is testable end to end offscreen against the real services with synthetic DNGs
  (`apps/lucent/tests/application_*_tests.cpp`), including staleness across threads and shutdown
  with renders, thumbnails and exports outstanding.
- Closing the window during a render or export waits for cooperative cancellation (a render
  stage, at most about a second on the R5).
- A Fit preview costs a full-resolution render (the engine has no reduced-resolution route);
  that dominates preview latency. A late request can wait behind an export's render and output
  conversion, which hold the single memory admission (about 3 s on the R5).
- Exports never overlap: the next one starts only after the previous one has written and
  verified its file, which lengthens a batch by the write time of each file. W7's durable batch
  scheduling revisits this with measured memory headroom.
- A 100 % preview of a 45 MP photo is a 180 MB `QImage` plus its texture; the preview cache
  holds up to 384 MB of previews and the thumbnail cache 48 MB.
- A file without a usable camera preview produces one Qt warning ("QML Image: ...") when its
  camera thumbnail fails, before it switches to Lucent's render.
- W6 must fold recipe and geometry revisions into the preview generation and decide whether the
  Edit view keeps the previous revision visible while the next one renders. W7 replaces the
  in-process renderer with worker processes. Display-profile support revisits decision 8.

## Alternatives considered

- **Qt-aware application services** (signals, `QThreadPool`). Rejected: the services must stay
  usable by workers and command-line tools without Qt (docs/architecture/overview.md), and a
  catalog or worker back end should not depend on the UI toolkit.
- **One preview channel per item.** Rejected: moving through the filmstrip would leave every
  earlier photo's render running; one channel for the Edit view makes the newest request cancel
  the rest.
- **Rendering Lucent thumbnails for every imported photo in the background.** Rejected for W3:
  each costs a full-resolution render (about 1 s for 45 MP) that would compete with the photo
  being edited for the single memory admission. Camera previews are shown instead and replaced
  as photos are opened; automatic batch rendering belongs to W7/W8.
- **Blocking hand-over to the UI thread** (`Qt::BlockingQueuedConnection`). Rejected: service
  destructors wait for their callbacks, so a callback blocked on the UI thread while the UI
  thread destroys the service would deadlock.
- **Keeping the simulated services in Lucent.exe behind an option.** Rejected: a shipped binary
  must not be able to present invented metadata; they remain a test double only.
