# ADR 0007: Application shell: UI adapters over in-process application services

- **Status:** Accepted
- **Date:** 2026-09-27 (revised the same day after the W3 review: executor lanes, serial exports,
  embedded-preview selection, import rules, labelling of camera and incomplete renders); amended
  2026-10-03 for the W6 editor (decisions 3, 4, 5 and 10 amended, decision 13 added); amended
  2026-10-04 for the W7 batch interface (decisions 3 and 10 amended, decision 14 added; decision 14
  revised by the W7 closing review: unfinished batches and paging, attentions outside the Exports
  view, keyboard selection, single-export marker)
- **Workflow:** W3 (Application shell); W6 (Recipes and editor), phase 3; W7 (Durable batch),
  phase 3

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
   scales a result down when it exceeds the requested box. *W6:* the edit adapter
   (`AppEditService`) runs recipe commands (catalog writes, file probes, session open and close)
   in call order on its own command thread and converts edit-session frames on its own conversion
   thread; the UI thread only stores the converted frame and publishes its URL (decision 13).
   *W7:* the batch adapter (`AppBatchService`) calls the durable batch service only from its own
   three single-thread pools (plans, submissions, commands and reads) and applies the service's
   events on the UI thread coalesced (decision 14).

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
     *W6 amendment.* Thumbnails of generation 2+ are Lucent's render of the item's current recipe
     (`PreviewService::request_rendered_thumbnail`, cached in memory and on disk by the settings'
     identity, ADR 0010 decision 9), and the image source's own thumbnail cache is keyed by that
     identity too, so an old recipe's thumbnail is never served for a new one. When an item's
     committed recipe changes (an edit, undo or redo, a paste) the library adapter moves its
     thumbnail to a new generation and sets its `edited` flag ("Edited" badge on the card, a dot
     in the filmstrip); at start-up the items of the catalog that have revisions are looked up in
     the background so edited photos show their edits from the beginning of a session. Opening a
     photo no longer replaces its camera-preview thumbnail: the Edit view shows edit-session frames
     (decision 13), not a PreviewService render, and a proxy frame is not the thumbnail's
     definition (ADR 0010); an unedited photo keeps its labelled camera preview until W7's batch
     renders it, an edited one gets Lucent's render of its recipe. The edit-session frames have
     their own staleness rule: every frame is a new URL (`image://frame/<item>/<sequence>`) and
     sequences only increase.
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
   *W6:* the Edit view's image is the edit session's frame: the 2048-pixel proxy in Fit (shown
   scaled by the GPU), and at 100 % a quick proxy frame (upscaled) followed by the
   full-resolution Final frame; 30 seconds after the view leaves 100 % the session's
   full-resolution stage one is released. Thumbnail renders also wait until interactive editing
   has not waited for a render for 1.5 s (`Backend::interactive()`), so a full-resolution
   thumbnail render never competes with a white-balance refine or the next drag.

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
    `--screenshot`, `--quit-after-screenshot` and `--settle-timeout` (W6 adds `--set
    <control>=<value>`, `--drag <control>`, `--drag-steps`, `--compare <before|split>` and
    `--clipping`: edits made once the photo's first frame is shown, a synthetic 60 Hz mouse drag
    of a slider with its frame latencies and UI-thread gaps, and the comparison and overlay for a
    screenshot; W7 adds `--export-batch <folder> --to <folder> [--preset jpeg|tiff]`, which starts a
    folder batch as the Export batch dialog does and reports its progress and outcome,
    `--show-batch-dialog` (open the dialog with those settings instead), `--screenshot-running
    <file.png>` (a screenshot while the batch runs) and `--quit-when-done`). Screenshots wait for idle
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

13. **Editing (W6, phase 3; ADR 0010).** The Edit view edits the photo it shows through a
    fourth UI-side service, `EditService` (`services/edit_service.hpp`), bound to `RecipeService`
    and `DevelopPreviewService` by `AppEditService`; the view model is `EditSession`
    (`models/edit_session.hpp`, `AppController.edit`), which follows the photo while the Edit view
    shows it (a session is opened for it and closed when the view or the photo changes).
    - *Controls.* White balance (temperature and tint when the camera's calibration defines a
      colour temperature, `WhiteBalanceModel::kelvin_available()`; otherwise the camera's own
      red-blue and green-magenta balance relative to as shot, with an explanation, and nothing for
      monochrome files), exposure, contrast, highlights, shadows, saturation and vibrance. Each has
      a slider (temperature on a reciprocal scale), a numeric field, reset (button, double-click,
      Home), arrow-key steps (Shift: coarse) and a lock toggle (`set_lock`, a revision), with
      accessible names; a slider's accessible value is the value its field shows (temperature,
      linear in reciprocal kelvin, reports its position and states the kelvin value in its
      description). Moving a white-balance slider from "as shot" starts a custom balance from the
      as-shot values.
    - *Gestures.* A drag is transient positions (`set_transient`, unsettled requests: Quick
      frames only) and one `commit` on release; a held arrow key is the same, committed on key
      release or after 600 ms without a key; a typed value, a reset or a curve point removal is one
      `apply`. So every gesture is exactly one revision. A command during a drag (undo, redo,
      paste, a reset by Home or double-click) commits what the drag changed so far, and the rest of
      the drag is the next gesture, committed on release: never a revision per mouse move.
      Positions are coalesced twice: on the edit adapter's command thread (a newer position
      replaces a waiting one) and in the develop service (latest wins). The local values are not
      overwritten by an older call's answer while a newer call is outstanding (states carry the
      call's token); while an undo, redo, selected revision, reset or paste is not answered yet,
      changes are refused (the service applies those commands, so a change built on the local
      values would re-apply what they removed). Commands still queued when Lucent closes are
      completed first (a gesture pending at exit is committed), only rendering stops.
    - *Tone curve.* `ToneCurvePlot` draws the curve as the renderer evaluates it
      (`EditService::sample_tone_curve`, the engine's monotone cubic) over the frame's luma
      histogram placed at the lightness of each code value; points keep x strictly increasing
      (0.01 apart) and y non-decreasing; the end points move but are not removed.
    - *Frames.* Quick frames are shown while settings change, then the Final frame; the previous
      frame stays on screen while the next one loads. The as-shot render for before/after (\ key;
      a split view whose divider is dragged, or focused with Tab and moved with the arrow keys)
      comes from the same session (an unsettled as-shot
      request made once the current settings are rendered), and the clipping overlay (J key) is
      the frame's clipping mask (red: a channel at 255; blue: a channel at 0), with the histogram's
      clipped shares counted from the same mask. The histogram follows the newest frame.
    - *History.* Every revision, newest first, with its origin, changed controls and time; a click
      selects it (`select_revision`), Undo/Redo (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z, and buttons),
      and Reset to as shot (a recorded revision). An item whose recipe this Lucent cannot read
      shows a read-only banner with the reason and no rendered image; its controls show no values
      (not the as-shot defaults, which would look like an unedited photo), edit controls, copy and
      paste are disabled, while undo, redo, selecting and reset stay available unless the whole
      library is read-only. Exports and other messages name the edit, not the file, as what cannot
      be read ("This photo's edit was made by a newer version of Lucent", "... is damaged").
    - *Copy and paste.* Ctrl+Shift+C opens a category dialog (white balance, exposure, tone,
      colour, highlights and shadows); Ctrl+Shift+V pastes into the photo. The result message
      (panel and status line) lists the categories pasted and, with the reason, anything not
      pasted: a camera-specific balance is never pasted to another camera model, a Kelvin balance
      never to a camera without a colour-temperature calibration. Auto is present but disabled
      until W8.
    - *Responsiveness.* Measured on the reference laptop with the 45 MP Canon EOS R5 fixture,
      Lucent.exe `--measure` with a synthetic 60 Hz mouse drag of 60 moves over a third of the
      slider (`--drag`), three runs each, 2026-10-03:

      | What | Result |
      |---|---|
      | Exposure drag: slider position to its frame on screen | median 66 to 68 ms, 90th percentile 72 to 77 ms, max 98 to 107 ms; 20 to 22 frames in the 1 s drag |
      | of which the develop service (request to frame ready) | median 56 to 59 ms |
      | Shadows drag (guided-filter local tone) | median 104 to 105 ms, 90th percentile 109 to 112 ms, max 138 to 142 ms; 12 to 13 frames |
      | Final frame after the release | 117 to 196 ms |
      | UI thread during the drags (5 ms timer, press to final frame) | largest gap 6 to 10 ms; none over 50 ms |
      | One-step edit (typed exposure or contrast) to its final frame on screen | 44 to 59 ms |
      | White balance 5200 K: quick frame / refined final frame | 44 ms / 1.07 to 1.18 s |
      | Opening the photo in Edit: first frame on screen | 0.81 to 1.06 s |
      | 100 %: full-resolution final frame on screen after the zoom | 1.75 s |

      The UI side (command thread, conversion, posting, image provider) adds about 8 to 10 ms
      to the develop service's latency.

14. **Batch export (W7, phase 3; ADR 0012).** A fifth UI-side service, `BatchService`
    (`services/batch_service.hpp`), is bound to `app::BatchService` by `AppBatchService`
    (`adapters/app_batch_service.hpp`). Its view models are `BatchSetupModel` (the Export batch
    dialog, `AppController.batchSetup`), `BatchListModel` (the Exports view, `batches`) and
    `BatchReportModel` (the selected batch's report, `batchReport`). Without a writable catalog the
    service reports itself unavailable with the reason, Export batch is hidden, and the Exports view
    lists the session's in-process export jobs as before (`ExportJobModel`).
    - *Selection.* The contact sheet keeps a multi-selection by item id: Ctrl+click toggles,
      Shift+click selects from the anchor, Ctrl+A selects every photo; from the keyboard
      Shift+Arrow / Home / End / Page Up / Page Down extend the selection from the anchor,
      Ctrl+Arrow moves the current photo without changing it and Ctrl+Space toggles the current
      photo; a plain click or a plain keyboard move selects the current photo only, and opening
      another photo elsewhere (the Edit filmstrip) keeps a selection of several photos. Selected
      photos are filled and outlined in the accent and reported selected to assistive technology;
      the current photo outside the selection only gets a thin neutral outline. Export batch
      (toolbar, Ctrl+Shift+E) opens the dialog with the selection when several photos are selected
      and with every listed photo otherwise (the library has no albums or filters yet: "all filtered
      items" is every listed photo); "Export a folder" (Library menu, the empty library's opening
      screen and the Exports view) selects a folder, which the service imports when the batch
      starts, so a folder can be exported without importing it by hand first.
    - *Dialog.* Photos (selected, all, or a folder with Include subfolders), output folder, naming
      pattern with token buttons and a local check as the user types, mirrored subfolders, the preset
      (JPEG / sRGB / quality 95, or 16-bit TIFF with sRGB or Display P3), "If a name is taken" (a
      number is added; "Replace the existing file" only after a confirmation dialog that names the
      folder, states that originals and the batch's own outputs are never replaced and defaults to
      keeping files; the choice is withdrawn when the folder changes and for every new batch), photos
      already exported (skip, the default, or export again), metadata (capture details, Artist /
      Copyright with the user's text, an empty field keeping the camera's value, and "Include GPS
      location", off each time the dialog opens), and Automatic settings shown disabled with
      "Available when Natural v1 is integrated". The plan panel stays in view below the scrolling
      settings: up to three example destination paths, outputs that will be written first (marked "a
      number is added" or "replaces the existing file"; an already exported one is marked "already
      exported: skipped"), the counts (exported, renamed, replaced, already exported, left out with
      reasons, originals to fingerprint first), the upper bound of the outputs' size with the drive's
      free space, and warnings in the user's terms worded from the service's stable plan notices
      (network or removable destination, a drive too full). A plan checks earlier outputs by size
      only (submission confirms them byte for byte), so a plan stays cheap however many outputs the
      folder already holds. Every change asks for a plan after 300 ms without further changes
      (typing a pattern makes one request); the newest request cancels the running plan and only its
      answer is shown.
      Start is enabled while a plan is computed (the service validates everything again when the
      batch is created) and disabled for a local problem or a plan error. The output folder, pattern,
      preset and the artist and copyright text are remembered in the per-user settings (`batch/*`);
      the GPS choice and a decision to replace files never are.
    - *Exports view.* The newest 100 batches and every batch with work left whatever its age (a
      paused batch is never lost behind newer single exports), newest first, with "Show older
      exports" paging further back; each with its state, live counts (exported, remaining, failed,
      skipped, left out at the start, for review), a progress bar, the running photos' stages
      ("Rendering IMG_0001.CR3 (42 %)"), an ETA only while the batch runs and the service has one,
      and Pause, Resume, Cancel, Retry failed (only while failed photos can be retried), Open report
      and Open output folder as they apply. A destination attention (full, with the free space and
      what the next photo needs; unavailable; permission; replaced) is a banner on the batch with
      what happened and what to do, naming the folder, and Resume, or "Use this folder" when another
      folder took the output folder's path. An attention is also posted to the status line and
      flags the Exports tab (an amber "!" badge, "needs attention" for screen readers) wherever the
      photographer is. A batch paused by the user (or found paused after a restart, when the service
      no longer knows why) reads "Paused. Choose Resume to continue." A single export (marked by the
      service, not guessed from its size) is a compact row: the photo's export, its file once
      written, its state, a "Review" mark when its receipt recommends one, and why it was not
      exported when the destination stopped it ("Not exported"); a batch of one from the dialog is a
      batch. The selected batch's report (the newest batch is followed unless the user chose
      another) lists every photo with its outcome, output, reason, suggested action and review
      notes, filtered by outcome (a newly selected batch starts with all photos), with Open output
      folder and Open report (the HTML report in the output folder, written on request when missing;
      for a single export in the report pane only). Reading the report again for the same batch
      updates its rows in place, so a list scrolled down keeps its place. A submission that is still
      importing a folder or fingerprinting originals is shown as "Preparing" with its step and
      Cancel. Batches started from the dialog report their start and outcome in the status line;
      single exports keep their job messages.
    - *Coalescing and responsiveness.* The service's observer runs on its coordinator and slot
      threads about a hundred times per photo; the adapter merges each event into a pending record
      per batch under a mutex and posts one wake-up while none is outstanding, and the UI thread
      applies the accumulated changes at most every 200 ms, one snapshot per changed batch. While a
      batch has work its status (ETA, running photos, attention, review and retry counts, left-out
      photos; a batch of one: its output) is read every second and once more as soon as a photo or
      the batch finishes; no report is read for the list. A status read that started before newer
      events does not overwrite their counts. The report pane reads the selected report again at
      most every 2 s (2 ms per photo for large batches, at most 15 s; the first change after a quiet
      interval at once) and a batch's final report as soon as it finishes. Plans, submissions (which
      import and hash), commands, status reads and reports run on the adapter's three single-thread
      pools, so a long submission never delays Pause or Resume. Measured on the reference laptop
      with the smoke fixtures, `--export-batch --measure`, 2026-10-04: the batch of five (four
      exported, the DJI DNG left out as Unsupported, one review note for the X-Trans baseline
      demosaic) took 14.1 s from process start to the final screenshot; the UI thread's largest gap
      between 5 ms timer ticks during the run was 91 ms (median 17 ms, four gaps over 50 ms, none
      over 100 ms). The adapter test runs a 24-photo batch through lucent-worker.exe and requires at
      most one application of events per 200 ms and no UI-thread gap of a second.

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
  *W6 (decision 13):* the Edit view shows edit-session frames (new URL per frame, the previous
  frame kept until the next is loaded) and thumbnails follow the recipe's identity; geometry
  revisions join the recipe in W9.
- Edited photos' thumbnails cost one full-resolution render after each committed change (after a
  1.5 s quiet period, cached by settings on disk for hashed items); unedited photos keep their
  camera preview after being opened in Edit.
- At 100 % with the clipping overlay on, a 45 MP frame holds about 180 MB for the image and as
  much for the overlay (only while the overlay is on).
- *W7 (decision 14):* the Exports view lists the newest 100 batches and every unfinished one (every
  single export is a batch); older finished ones are paged in with "Show older exports". Report
  files exist for batches started from the dialog; single exports write none unless Open report is
  chosen in the report pane. Live counts lag the service by up to 200 ms, the ETA, review and retry
  counts by up to a second. The dialog's examples cover at most three photos.

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
