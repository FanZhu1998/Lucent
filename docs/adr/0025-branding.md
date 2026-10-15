# ADR 0025: Lucent's logo: one master, generated derivatives found by role, and the icon in the programs, the window and Setup

- **Status:** Accepted (amended 2026-10-15 by the W12 release review: "Amendment: the branding review"
  below; the asset is branding 1.0.1)
- **Date:** 2026-10-14
- **Workflow:** W12 (Release hardening), release phase: branding
- **Amends:** ADR 0009 (Setup's icon and wizard images, the programs' icon and version resources, and
  the packaging's checks of them; see its amendment "Lucent's icon"). Uses ADR 0018's asset manifest
  format for an asset that is used at build time and never installed.

## Context

Lucent had no logo. `Lucent.exe` and `lucent-worker.exe` carried no icon resource, so Explorer, the
taskbar, the Start Menu shortcut and the uninstall entry showed Windows' generic program icon; Setup
showed Inno Setup's own icon and wizard images; Help > About was text only.

On 2026-10-11 the project owner, Fan Zhu, supplied a PNG as the application's logo and asked to "use
the attached png file as the App logo" and to "modulize updates in a forward compatible way". The
image (verified 2026-10-11 and again 2026-10-14):

- 1,013,687 bytes, SHA-256 `106aaee47ba11568677fe96d509d903280be72227befc9d34edffa49437eab43`; a PNG of
  1254 x 1254 pixels, 8-bit RGBA, not interlaced, with the chunks IHDR, caBX, IDAT and IEND.
- A fully transparent background around a dark rounded square (about `#18252B`) holding a two-tone
  "L": a teal vertical stroke (about `#ABD7CF`, with a slight gradient) and a pale foot (about
  `#E3F1EC`) meeting at a 45-degree mitre. The square spans x 97 to 1156 and y 107 to 1171 (1060 x 1065
  pixels by the bounding box of alpha 128 and more: 0.4 % taller than wide), about 12 pixels below the
  centre.
- Its interior is not quite opaque: alpha 254 (1,055,679 pixels) and 253 (27,548), with a soft inner
  rim of 241 to 252 within 11 pixels of the edge and 2,520 pixels of 255. Outside the square there are
  generator artefacts: 1,977 pixels of alpha 1 and 2, a faint ring about two pixels outside the edge
  (alpha up to about 9), and specks as far as x 1227 and y 25. The anti-aliased edge pixels carry the
  plate's colour, darker only where their alpha is below about 32. The L's edges show the image
  generator's sharpening (a lighter line inside the stroke's edge, a darker one outside), one or two
  pixels wide.
- The `caBX` chunk (21,881 bytes) is a C2PA content-credentials manifest (C2PA 2.2): claim generator
  "OpenAI Media Service API"; actions `c2pa.created` by ChatGPT (gpt-image) at 2026-10-10T17:48:31Z with
  the digital source type `trainedAlgorithmicMedia`, `c2pa.converted` and `c2pa.watermarked.unbound`;
  signed by "OpenAI Media Service" (OpenAI OpCo, LLC; certificate issued by Trufo C2PA Claim Signing CA
  (2025)), time-stamped 2026-10-10T17:48:31Z. These facts are read from the manifest's text; nothing in
  Lucent's toolchain validates a C2PA signature.

The places the logo must appear, and what each takes:

- **Program icons.** Windows shows a program's first icon group (`RT_GROUP_ICON`): Explorer, the
  taskbar, shortcuts without an icon of their own, `"<program>,0"` references such as Setup's
  `UninstallDisplayIcon={app}\Lucent.exe` and the "Open with" ProgID's `DefaultIcon`. Qt's Windows
  platform plugin registers its window class with the icon resource named `IDI_ICON1`
  (`QWindowsContext::registerWindowClass`, `LoadImage(..., L"IDI_ICON1", ...)`). The resource compiler
  (RC.exe) takes an `.ico` file; Windows 10 and 11 read 32-bit images with alpha in it, as BMP or PNG.
- **The window icon.** `QGuiApplication::setWindowIcon` with a `QIcon` of every size; Windows asks for
  the small (16 logical pixels) and large (32) icon at the display's scale. Qt reads PNG without a
  plugin; an `.ico` needs its `qico` image plugin.
- **Help > About.** A Qt Quick `Image`: with a `sourceSize`, Qt Quick asks an image provider for
  `sourceSize` times the device pixel ratio it knows when the image loads, and shows the result at the
  logical size (Qt 6.11.1, `QQuickImageBase::loadPixmap`). That ratio is the window's when the item is
  in a window, otherwise the largest ratio of all screens, and a loaded image is not reloaded when its
  item later enters a window. The reference laptop has three screens, at 125, 150 and 150 %: a logo
  loaded while the About dialog was closed was a 96-pixel image, scaled down to 80 device pixels in the
  125 % window. And a whole logical position is a fractional device position at 125, 175 and 225 %:
  the dialog, centred in logical pixels, put the logo half a device pixel off, so even the right image
  was resampled.
- **Setup** (Inno Setup 6.7.3, `WizardStyle=modern`, its help file `ISetup.chm` read on 2026-10-14):
  `SetupIconFile` is Setup's and the uninstaller's icon, recommended with at least 16, 32, 48, 64 and
  256 pixels; `WizardImageFile` (the Welcome and Finished pages) and `WizardSmallImageFile` (the other
  pages' header) take `.bmp` or `.png` "including those with transparency", and a comma-separated list
  from which Setup picks the image that best matches its image area: 202x386, 269x515, 336x643,
  403x772, 430x824, 498x953 and 534x1022 pixels at 100 to 250 % for the large image (aspect 164:314),
  58, 77, 97, 116, 124, 143 and 159 pixels for the small one. A custom wizard image's background
  colour defaults to the window colour.
- **The repository's README.**

The repository's rules apply: versioned contracts that readers refuse when newer (conventions,
"Versioned contracts"); data assets with a "lucent.asset-manifest" manifest of every file's size and
SHA-256 (ADR 0018); deterministic outputs; Windows PowerShell 5.1 scripts, C# through `Add-Type` where
speed matters (as `installer\scripts\LucentCompressedContent.cs`), no new third-party dependency and no
Python in the product path; nothing a release ships without a recorded licence (ADR 0024).

A first attempt at this work (2026-10-11) wrote the generator and the images and was interrupted
before any integration; this phase reviewed it, kept its image pipeline (checked numerically below),
fixed two defects (the SemVer comparison failed under strict mode for two release versions, and reading
an executable's icons threw on an image of another form), added the provenance record, and did the
integration, the tests and the documentation.

## Decision

1. **One asset, one master, generated derivatives.** `assets/branding` holds:
   - `master/lucent-logo.png`, the owner's image byte for byte, content credentials included;
   - `provenance.json` ("lucent.branding-provenance" version 1), written by hand: the master's path,
     size and SHA-256, what it shows, who supplied it and when, how it was made (from its content
     credentials), modifications (none), its licence (`null` until the owner decides, decision 12) and
     the state of that decision;
   - the derivatives, each with a **role**, the stable contract with its consumers: `windows-icon`
     (`windows/lucent.ico`), `app-icon` (`app/lucent-<N>.png`), `setup-wizard-image`
     (`installer/wizard-image-<W>x<H>.png`), `setup-wizard-small-image`
     (`installer/wizard-small-image-<N>.png`) and `docs-logo` (`docs/lucent-logo.png`);
   - `branding.json` ("lucent.branding" version 1, generated): the master (path, format, size,
     SHA-256, chunks, provenance record), what the analysis found, the method in words, and every
     derivative with its role, format, pixel size and the square its artwork fills (for the icon, each
     image with its encoding);
   - `manifest.json` ("lucent.asset-manifest" version 1, id `branding`, content "lucent.branding"
     version 1; ADR 0018's format and canonical text): every file but itself and the README with its
     size and SHA-256;
   - `README.md`: what each file is for, how the images are made, how to replace the logo, the
     provenance and the licence state.

   The asset is used at build time and never installed: the images are inside `Lucent.exe`,
   `lucent-worker.exe` and Setup. Consumers find derivatives by role, never by file name or size.

2. **The generator.** `tools\branding\Update-Branding.ps1` (module `LucentBranding.psm1`, image code
   `LucentBranding.cs`, C# 5 through `Add-Type`, no GDI+, no third-party code) renders every derivative
   from the master in five steps: validate (8-bit RGBA PNG, 512 to 8192 pixels a side, sRGB: no `iCCP`,
   `cHRM`, `gAMA`, `sBIT`, `tRNS` or animation chunk; the artwork away from the edges and at most 1.25:1),
   the alpha rule (decision 3), the content box (the bounding box of alpha 128 and more) centred in each
   derivative's artwork square with its longer side spanning it (the master's off-centre placement is
   corrected, its proportions kept), area averaging (decision 4) and encoding (decision 5). Writing:
   `-MasterFile` imports a new master byte for byte after validating it, `provenance.json` must already
   describe it, files whose bytes do not change are left alone, files the previous manifest listed and
   the recipe no longer makes are deleted, and any change needs `-Version` with a higher branding
   version (SemVer: MAJOR when a role or file is removed or renamed, MINOR for a new master or new
   derivatives, PATCH for the same master rendered differently). `-Check` writes nothing: the folder must
   hold exactly the listed files (and the README) with their sizes and SHA-256, the provenance record
   must describe the master, and a fresh rendering must reproduce every derivative, `branding.json`
   and `manifest.json` byte for byte; a difference names the file and how many pixels differ. Every
   reader (the generator, CMake, the tests) refuses a manifest, content or provenance schema version it
   does not know, and the generator never overwrites another schema. The recipe, the one table of
   derivatives, is `Get-BrandingRecipe`.

3. **Alpha: the artwork is made opaque.** Alpha 1 and 2 are cleared as noise, then every 4-connected
   group of non-transparent pixels without a pixel of alpha 128 or more (specks, and the faint ring
   separated from the edge by transparent pixels; an artwork of several parts keeps them all, since each
   has a solid core). The solid interior (content pixels whose 7 x 7 neighbourhood is all content) gives
   the 1st-percentile alpha, 253 for this master; it must be at least 240, and alpha becomes coverage
   min(1, alpha / 253): the nearly opaque interior is opaque, the anti-aliased edge keeps its ramp. A
   translucent artwork is refused: it needs a rule of its own, not this one.

4. **Resampling: area averages of premultiplied linear light.** Each output pixel is the exact
   footprint-weighted average (a box filter over the master pixels it covers) of premultiplied colour in
   linear light, sRGB decoded and encoded with the IEC 61966-2-1 curve computed with additions,
   multiplications and divisions only (no C runtime `pow`), and rounded in the encoded domain. Alpha is
   the covered fraction, colour the average colour of what covers it: a transparent neighbour never
   darkens an edge, two colours average as light does, and a box filter cannot ring. The same pixels
   come out on every machine.

5. **Encoding.** PNGs with IHDR, IDAT and IEND only (8-bit RGBA; no metadata, no content credentials,
   no time), rows filtered by the smallest sum of absolute values and one zlib stream with fixed
   Huffman codes: the same pixels always give the same bytes, so the check compares bytes. The icon
   file holds the sizes 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256: 32-bit BMP images
   (BITMAPINFOHEADER, alpha not premultiplied, bottom-up rows, an AND mask set exactly where alpha is
   0) below 256 pixels, the form every reader takes, and a PNG at 256 pixels (50 KB instead of 263 KB).
   Each icon image has the pixels of the `app-icon` PNG of its size.

6. **Sizes and padding.** Icons keep round(size / 16) transparent pixels on each side (1 at 16 and
   20 pixels, 2 at 24 to 36, 3 at 40 and 48, 4 at 56 and 64, 16 at 256): the artwork fills seven eighths
   of the icon, starts on a whole pixel and lines up with the plated icons around it. The `app-icon`
   sizes are every size the window's small and large icons (16 and 32 logical pixels) and the About
   logo (64) need at the display scales Windows offers from 100 to 300 % (16 to 48, 32 to 96, 64 to
   192 pixels: 19 sizes with 256). The wizard images have exactly Inno Setup 6.7.3's image areas; the
   large one is transparent with the logo at half the panel's width, centred, its middle at 38 % of the
   panel's height (on the light `modern` wizard it stands in the window colour above the welcome text);
   the small one is icon-style. The docs logo is 256 pixels with a 4-pixel margin.

7. **The programs' resources.** `cmake/LucentBranding.cmake` (included by the root `CMakeLists.txt`)
   reads `manifest.json` and `branding.json` (refusing other schemas), checks every derivative's size
   and SHA-256 against the manifest (a derivative edited by hand stops the configure step), makes them
   all configure dependencies and gives the files of a role (`lucent_branding_files()`).
   `lucent_add_program_resources(<target> DESCRIPTION <text>)` configures the one template
   `cmake/lucent-program.rc.in` for a shipped program: the icon `IDI_ICON1` (a name, so Qt finds it)
   and the version information (company, description, versions with the commit, MIT copyright).
   `Lucent.exe` and `lucent-worker.exe` both use it; it replaces `apps/lucent/lucent.rc.in`, which held
   Lucent.exe's version block alone, so the worker gains the same version information without a second
   copy of it. The icon is an `OBJECT_DEPENDS` of the resource script: a new icon rebuilds both programs.

8. **The application.** `apps/lucent/CMakeLists.txt` compiles the `app-icon` PNGs into the QML module's
   library as the Qt resource folder `:/lucent/branding/app-icon` under their own file names.
   `apps/lucent/src/branding.hpp` reads every image of that folder: `branding_icon()` is a `QIcon` with
   every size, which `load_main_window()` makes the application's window icon (no `qico` plugin
   involved), and the image provider `branding` serves `image://branding/<role>` at the requested size:
   the pre-rendered image of exactly that size, or the next larger one scaled down, never one scaled up.
   Help > About shows the logo beside its title through an `Image` with `sourceSize` 64 x 64: at every
   scale from 100 to 300 % in Windows' steps the provider has the image of exactly 64 times the window's
   device pixel ratio, which Qt Quick draws 1:1. Two rules make that hold on every screen: the image
   loads only while the dialog is in a window (`source` is bound to `Window.window`), so the ratio Qt
   Quick asks for is that window's, and the dialog is centred on whole device pixels (its position is
   rounded in device pixels; its padding, 20 logical pixels, is whole at every scale). Nothing names a
   size or a file name.

9. **Setup.** `installer\lucent.iss` takes `SetupIconFile`, `WizardImageFile` and `WizardSmallImageFile`
   from the defines `BrandingIcon`, `BrandingWizardImages` and `BrandingWizardSmallImages`, which
   `tools\package.ps1` passes from the asset by role (every wizard size, comma-separated); the script
   stops when one is missing. The uninstall entry (`UninstallDisplayIcon={app}\Lucent.exe`), the
   shortcuts and the "Open with" ProgID's `DefaultIcon` keep pointing at `Lucent.exe`, whose first icon
   group is now the logo. Inno Setup copies the icon's images into Setup's `MAINICON` byte for byte and
   stores no source path.

10. **Packaging checks it, and nothing new ships as a file.** `tools\package.ps1` reads and verifies the
    asset before the slow steps (`Read-BrandingAsset`: schemas, every file's size and SHA-256), checks
    after the smoke run that the staged `Lucent.exe` and `lucent-worker.exe` have `IDI_ICON1` as their
    first icon group with exactly the icon's images (`Test-ProgramIcon`, read with `LoadLibraryEx` as a
    data file), and after compiling that Setup's first icon group is the icon (a Setup that fails is
    deleted). The images are resources inside the executables and Setup, so the installed layout, its
    audit, the installed-file list, the SBOM, the notices and the licence policy are unchanged; the
    builder-path scan covers the new resources like every other byte (and decompresses the PNGs' zlib
    streams).

11. **The README** shows the `docs-logo` derivative at the top.

12. **Licence: decided by the owner, not here.** The provenance facts are recorded (`provenance.json`,
    the asset's README); the logo's licence terms are not: `provenance.json` says `"licence": null` and
    why. The release checklist's licence review (gate 8) gains the item "the logo's licence terms are
    decided by the owner and recorded"; until then no terms are claimed for the logo, and no release is
    published.

13. **Forward compatibility.** The master is the one source of truth; every image is regenerated from
    it; consumers depend on roles and the folder's stable names, not on sizes. Replacing the logo is a
    new master, its provenance record, one run of the generator with a new version and a commit of the
    folder (the asset's README gives the steps); no code changes. Adding a size or a target is an entry
    in the recipe and a new MINOR version; a new role also needs its consumer to ask for it by role.
    A **vector master** fits in without consumer changes: `branding.json` records `master.format`, and a
    generator that reads SVG rasterises it into the same premultiplied linear-light master it renders
    from today (for example with Qt's `QSvgRenderer`, already a dependency through `qtsvg`, in a small
    developer tool the generator calls, at four times the largest artwork size or more), or renders each
    small size from the vector directly; the roles, paths, manifest and consumers stay as they are. A
    change that readers would misread (a new derivative form, another alpha rule a consumer must know
    about) is a new content schema version, which today's readers refuse.

14. **Tests.** CTest `lucent_branding_tests` (`tests\branding\Test-Branding.ps1`, no build needed): the
    committed asset passes `-Check`; the icon's structure (sizes, depth, BMP and PNG images, the AND mask,
    alpha, Windows' own reading of every image) and its pixels against the `app-icon` PNGs; every PNG's
    chunks, size and transparent corners; the `app-icon` sizes and the wizard sizes against the
    requirements; generating from the master alone reproduces every committed file byte for byte;
    tampering with a copy is found (a changed byte, a re-encoded image with its manifest entry updated, a
    missing or added file, the provenance of another master, schema version 2); synthetic masters show the
    rules (centring, cleared specks and ring, opacity, no dark fringe on a white edge, linear-light
    averaging, sRGB 188 for half black and half white) and the refusals; a new master needs its provenance
    and a higher version; the contact sheet draws. CTest `lucent_program_icon_tests`
    (`Test-ProgramIcons.ps1`): the built `lucent-worker.exe` and `Lucent.exe` have exactly one icon group,
    `IDI_ICON1`, the icon image for image, and Lucent's version information; the sheet of their icons
    draws. CTest `lucent_setup_branding_tests` (`Test-SetupBranding.ps1`, a packaging check without a
    release build): `installer\lucent.iss`, compiled by ISCC with the branding defines and stand-ins for
    the installation folder and the runtime installer, makes a Setup whose only icon group is the icon,
    image for image, and whose decompressed setup data holds every wizard image byte for byte; without the
    defines it does not compile (skipped where no ISCC is found). `lucent_ui_tests`
    (`QmlShellTest.TheWindowIconAndTheAboutLogoAreLucentsLogoAtEveryScale`): the window icon has every
    size, each image as built; the provider's images; the About logo loads at 64 x 64 and is drawn 1:1.
    `lucent_assets_tests` (`AssetManifest.TheBrandingAssetsManifestIsCanonicalAndComplete`): the C++
    manifest reader accepts the generated manifest, its writer writes the same text, and it lists every
    file of the folder.

## Consequences

- Lucent shows its logo wherever Windows and Setup show a program: Explorer, the taskbar, the Start
  Menu shortcut, the uninstall entry, Task Manager (both programs), the window and Help > About, and
  Setup's file, window and wizard. The worker gains version information.
- The logo's images are reproducible and checked: a hand edit, a stale image or a wrong manifest entry
  fails the configure step or CTest, and packaging refuses programs or a Setup without the icon.
- About 1.8 MB of images enter the repository (the master 1.0 MB, the derivatives 0.8 MB); each new
  logo adds a new set to the history.
- Regenerating needs Windows PowerShell 5.1 and nothing else; the C# is compiled at run time.
- The dark plate has little contrast on a dark taskbar; that is the artwork's design and is kept (only
  the owner changes the artwork). The 16-pixel icon keeps the L legible: its stroke is about 2.7 pixels.
- **Owner's obligation:** decide the logo's licence terms and record them before the first release
  (gate 8). The master was made with an image generator; whatever terms the owner chooses, the
  provenance record states how it was made.
- Revisit when a vector master, a dark-mode variant, a monochrome taskbar or tray icon, or MSIX
  packaging (which wants its own tile images) is wanted: each is a new role and recipe entry.

## Alternatives considered

- **Hand-made icons in an image editor.** Not reproducible, not checkable, and every new logo would be
  manual work again.
- **ImageMagick, Pillow or Inkscape.** New third-party tools on the release machine (and Python in the
  product path), with results that change between their versions.
- **System.Drawing (GDI+) for the resampling.** It filters in gamma-encoded sRGB, its bicubic modes ring,
  and its PNG encoder adds chunks and is not byte-stable between Windows versions. The contact sheet,
  which is evidence and not a derivative, does use it.
- **A sharper filter (Lanczos, bicubic).** Crisper at small sizes, but it rings: halos around the pale L
  on the dark plate, which the task and the icon's small sizes cannot afford. The box filter cannot
  ring, and in linear light the L stays bright and legible.
- **Keeping the master's alpha 253 to 254.** The icons would let 1 % of the background through their
  interior, inconsistently with their AND masks (which call those pixels opaque), for no visible
  benefit; the master's alpha is generator noise, not a design decision.
- **An icon of PNG images only.** Smaller, and read by Windows 10 and 11, but BMP below 256 pixels is
  the form every reader takes (resource compilers, `LoadImage`, Inno Setup's icon update, Qt's `qico`
  plugin); PNG is used where it saves the most.
- **The window icon from the `.ico` through Qt's `qico` plugin.** It depends on a plugin loaded by name at
  run time; the PNGs need none.
- **One 256-pixel PNG for the About dialog, scaled by Qt Quick.** Qt scales in gamma-encoded sRGB and
  at fractional scales the image would be resampled twice; the provider's pre-rendered sizes are drawn
  1:1.
- **Keeping `apps/lucent/lucent.rc.in` and a second script for the worker.** Two copies of the version
  block; one template for every shipped program has none.
- **No large wizard image, or a dark full-panel one.** A blank panel looks unfinished; a dark panel is a
  new composition (a design decision beyond the master), and with `WizardStyle=modern` a transparent
  image on the window colour matches the page.
- **Recording the branding in the package manifest.** The images are inside the programs, which the
  manifest lists with their SHA-256; packaging checks the icons. A field can be added when a reader
  needs it.

## Evidence (2026-10-14, the reference laptop)

- **The images.** The analysis cleared 1,977 pixels of alpha 1 and 2 and 296 faint groups of 3,132
  pixels; the content box is 1060 x 1065 at (97, 107); the solid alpha is 253. In every `app-icon` PNG the
  alpha centroid is within 0.08 pixel of the centre (the box is centred exactly; the rest is the
  artwork's own asymmetry); the deviation of any edge pixel from the plate's colour, weighted by its
  alpha, is at most 2 levels of 255 (no dark fringe); the icon's images equal the PNGs of their sizes
  pixel for pixel, and an independent reader (Pillow) reads all ten. Generating everything takes about
  5 seconds, and `-Check` the same; a generation from the master alone in an empty folder reproduced the
  35 derivatives, `branding.json` and `manifest.json` byte for byte.
- **Inno Setup 6.7.3.** A throwaway script with `SetupIconFile` and the seven-image lists of both wizard
  images compiled ("Updating icons (Setup.exe)"); Setup's only icon group, `MAINICON`, held the ten
  images byte for byte, the wizard PNGs were in its setup data byte for byte, and the builder-path scan
  of that Setup (its LZMA parts decompressed) found no path of the build machine: ISCC stores neither
  the icon's nor the images' source path.
- **The package** (`tools\package.ps1 -BuildDir build\wl-brand-rel -Jobs 4 -UpdateChannel none`, the
  release preset built from scratch, vcpkg restoring its 39 packages from the binary cache, 1,188 s): the
  layout audit passed (344 files, 70 PE files; 0 errors, 0 warnings; the bundled assets verified), the
  smoke run exited with 0 without a Qt or QML loading error, the staged `Lucent.exe` and
  `lucent-worker.exe` carried `IDI_ICON1` with the icon's ten images byte for byte, the licences, the
  corresponding-source archive (132,443,744 bytes, 110 files, the same size as before) and the SBOM
  (107 packages, 545 files; structural check, no `pyspdxtools` on this machine) were written, the
  builder-path scan found nothing in 494 files with 975 compressed parts decompressed (the installation
  folder's parts rose from 478 to 499: the PNGs inside the two programs), and Setup (44,409,547 bytes;
  43,685,562 for the package of 2026-10-10) compiled with the icon and the wizard images (ISCC read all
  14 images and updated the icons of Setup and of its setup engine, which becomes the uninstaller), its
  only icon group `MAINICON` the icon byte for byte. The package manifest records `audit.errors` 0 and
  `builderPaths.hits` 0 (and `gitDirty` true: nothing of this phase was committed yet).
- **The About dialog** (`build\wl-brand-evidence\about-dialog*.png`, `Lucent.exe --show-about
  --screenshot`, from the build tree with a temporary `--data-root`). On the 125 % screen the logo first
  differed from the 80-pixel image by up to 122 levels (half a device pixel off), then, with the dialog
  on whole device pixels, by up to 44 (the image loaded at the 150 % screens' ratio: the 96-pixel image
  scaled down, which matched within 16); with the logo loaded in the window, it matches the 80-pixel image
  composited on the dialog within 0.9 levels. With `QT_SCALE_FACTOR` 1.4 and 1.8 (175 and 225 %) it
  matches the 112- and 144-pixel images within 1.0 and 0.9. The packaged `Lucent.exe` (the release
  build's installation folder) gives the same result at 125 %.
- **The window icon** (`build\wl-brand-evidence\window-icon.png`, the `dev` build's `Lucent.exe` with a
  temporary `--data-root` on the 125 % screen). Windows took 20- and 40-pixel icons (`WM_GETICON`),
  which Qt made from `app/lucent-20.png` and `app/lucent-40.png`: the same alpha and transparency mask,
  and the colour equal within one level premultiplied (Qt keeps pixmaps premultiplied, so the straight
  colour of a few faint edge pixels differs more); the window class's icons, loaded from `IDI_ICON1`,
  equal those PNGs pixel for pixel. Drawn by `DrawIconEx` on a 32-bit bitmap, all four match the PNGs
  composited on the same grey within 2 levels.
- **The configure step.** A throwaway CMake project including `cmake/LucentBranding.cmake` over a copy of
  the asset configured (19 `app-icon` files); with one byte of `app/lucent-32.png` changed it stopped
  ("... is not the file ... manifest.json lists"), and with the manifest's `schema_version` 2 too.
- **The programs.** In the `dev` build both resource objects depend on `windows/lucent.ico`, the Qt
  resource on every `app-icon` PNG and the configure step on every derivative (`build.ninja`): a new
  image rebuilds what uses it. The shell's own extraction of `Lucent.exe`'s icon
  (`ExtractAssociatedIcon`, 32 pixels) equals `app/lucent-32.png` pixel for pixel, and Explorer's image
  factory (`IShellItemImageFactory`) gives 16-, 48- and 256-pixel images of the `.ico` file and of both
  programs.
- **The icons as Windows draws them** (`build\wl-brand-evidence\program-icons.png`,
  `New-ProgramIconSheet`: a Setup compiled from `installer\lucent.iss` with the branding defines and a
  stand-in payload, and the `dev` build's `Lucent.exe` and `lucent-worker.exe`). Drawn by `DrawIconEx` on
  a 32-bit bitmap at 16, 32, 48 and 256 pixels on three greys, every program's icon matches the
  `app-icon` PNG of that size composited on the same grey within 1 level. The sheet's first version drew
  on a 24-bit GDI+ bitmap, where `DrawIconEx` drew the soft edge pixels at full opacity (up to 228
  levels off): its jagged corners were the drawing's, not the icon's; the sheet now draws on a 32-bit
  bitmap.
- **Tests.** `lucent_branding_tests`: 230 checks; `lucent_program_icon_tests`: 19 checks with the `dev`
  build's `Lucent.exe` and `lucent-worker.exe` (each: `IDI_ICON1`, ten images byte for byte, "Lucent RAW
  photo developer" or "Lucent processing worker", 0.1.0 with the commit; and the sheet of their icons
  draws); `lucent_setup_branding_tests`: 20 checks with ISCC 6.7.3 (Setup's `MAINICON` byte for byte,
  all 14 wizard images in the setup data, "BrandingIcon is not defined" without the defines), under 10
  seconds. The contact sheet's and the icon sheet's checks were added after the test runs below, with
  the 32-bit drawing; `ctest -L branding` in the `dev` build then passed all three.
- **The test runs** (`tools\build.ps1 -Test`, six jobs). `dev` (`build\wl-brand-dev`): 2,305 tests,
  2,241 passed, 57 fixture tests skipped by their tier, 6 disabled, 1 failed: `lucent_bench_harness_tests`
  ("no process of an ended run is left running": a process of the run the harness ends after a
  one-second timeout was still recorded as running, with six tests in parallel); it passed in this
  phase's first `dev` run, in the `dev-core` run and three times out of three alone, and nothing of the
  benchmark harness changed. `dev-core` (`build\wl-brand-core`): 2,025 tests, 1,962 passed, 57 skipped,
  6 disabled.

## Amendment: the branding review (W12 release review, 2026-10-15)

The review of this change (findings BRD-01 to BRD-12; BRD-07, BRD-08 and BRD-11 concern the
programs' resource script and the About dialog: items 24 to 26) found the generator's write mode
trusting its manifest, two promises of decision 13 broken by the tests, a gap in decision 2's master
rules, and statements that were not quite true.

15. **The generator deletes nothing outside the asset** (decision 2; BRD-01). Write mode deleted every
    file the previous `manifest.json` listed and the recipe no longer made, with no check of the path:
    a manifest naming `../../tools/package.ps1` would have deleted that file (the review deleted a file
    outside a copy of the asset this way), and check mode's advice for an invalid path ("Regenerate it
    with ... -Version") led there. Every path of the previous manifest must
    now pass the asset's path rule (`Test-BrandingPath`: relative, lower-case segments of letters,
    digits, `.`, `_` and `-` that start and end with a letter or digit, so never `.` or `..`, no drive,
    no backslash) or nothing is written or deleted ("restore manifest.json from git"); a deletion also
    resolves the full path and requires it inside the folder, never `manifest.json` or the README.
16. **Check mode reports each problem on its own and says what to do about it** (BRD-05). A deleted
    derivative was one problem counted, two printed (the second, "branding.json names ..., which
    manifest.json does not list", false), and the advice, a new `-Version`, was refused by the
    generator ("Nothing changed"). Now each problem is reported and counted once, a file listed but
    missing is not also called unlisted, and the advice depends on the problem (`Get-BrandingRemedy`):
    a missing or damaged derivative is written again by the generator without `-Version` (the version
    stays, and the generator says what it restored), an unlisted file is deleted, a changed
    `provenance.json` needs a higher version, a damaged manifest or record is restored from git.
17. **Hidden and system files count** (decision 2's "exactly the listed files"; BRD-04). The asset
    reader skipped hidden and system files, which `-Check` and packaging then passed while the C++
    check (`std::filesystem`) refused them, and git commits them without the attribute. It now sees
    them (`Get-ChildItem -Force`) and refuses every unlisted file, `desktop.ini` and `Thumbs.db`
    included (git ignores those two, so they never reach a clone, but both checks refuse them locally).
18. **The master's chunks are a list of what is allowed** (decision 2; BRD-06). The refused chunks
    were listed one by one, and PNG's third edition adds `cICP`, which a viewer applies before `iCCP`
    and `sRGB` (Display P3, BT.2020 with PQ), and the HDR chunks `mDCV` and `cLLI`: such a master
    passed and was rendered as sRGB. A master's ancillary chunks may now only be `sRGB`, `caBX` (the
    content credentials) and metadata (`tEXt`, `zTXt`, `iTXt`, `pHYs`, `tIME`, `eXIf`, `bKGD`,
    `sPLT`); every other one, a future one included, is refused.
19. **A change of `provenance.json` alone is a PATCH** (decisions 2 and 12; BRD-03). Recording the
    logo's licence, as gate 8 asks, changes `provenance.json`, which the manifest lists, so `-Check`,
    the build and packaging refuse the folder until it is generated again, and the versioning rules
    had no case for it. A change of `provenance.json` or of the generated record's wording only is a
    PATCH; the release checklist's gate 8, the asset's README ("Licence") and the generator's
    `-Version` message give the steps: edit `provenance.json` and the README, run
    `Update-Branding.ps1 -Version <next PATCH>`, commit, and start the release at gate 1 with that
    commit.
20. **The tests take their facts from the asset** (decision 13; BRD-02). `Test-Branding.ps1` named the
    current master's size (1,013,687 bytes) in one check and compared the icon with the exact list of
    its ten sizes, so a new logo, or a size added to the recipe, failed CTest until the test was
    edited. It now takes the master's size from the asset and the icon's sizes from `branding.json`
    (the `windows-icon` images), and requires only that they include 16, 32, 48 and 256 pixels, which
    Windows and Inno Setup recommend. Replacing the logo, or adding a size, again changes no code.
21. **Two statements corrected** (decision 6 and the Context; BRD-12). Setup shows the large wizard
    image on its Finished page only: Inno Setup 6 disables the Welcome page by default
    (`DisableWelcomePage`), and `installer\lucent.iss` keeps that, so the image does not stand "above
    the welcome text". And the icons' margin is not quite transparent: the content box (alpha 128 and
    more) keeps round(size / 16) pixels from each edge, and the artwork's anti-aliased rim, below alpha
    128, reaches about one pixel into that margin (alpha up to 26 at 256 pixels, 2 at 16). The method
    text in `branding.json` says so; that changed the generated record, so the asset is now branding
    **1.0.1** (a PATCH: every image is byte for byte what 1.0.0 had).
22. **Small icons stay soft** (BRD-10). At 16 to 24 pixels the L's edges fall between pixels (its
    stroke is about 2.7 pixels wide at 16), so they show partial columns: legible, slightly soft next to
    hand-hinted icons. Fitting the edges to the pixel grid would trade exact centring for crisp edges
    and change every small image; it is not done, and is the owner's call when a new master or a
    hinted small-size master is made.
23. **The logo is outside the MIT grant until its terms are recorded** (decision 12; BRD-09). The
    repository's README and the asset's README say that the logo in `assets/branding` is not covered by
    the MIT License of Lucent's source code; its terms are decided by the owner and recorded at gate 8.
    No terms are stated, and `LegalCopyright` in the programs is unchanged.
24. **The resource script is UTF-8 and says so** (decision 7; BRD-07). `configure_file()` writes the
    script in UTF-8 with the icon's absolute path, and RC read it in the system's ANSI code page: in a
    checkout under a folder whose name is not ASCII (a user name with an accent, a folder named in
    Chinese) both programs' resource step failed (`error RC2135 : file not found`).
    `cmake/lucent-program.rc.in` now starts with `#pragma code_page(65001)`; the version strings are
    ASCII and compile as before. CTest `lucent_program_resource_script_tests`
    (`tests\branding\program_resources_test.cmake`, label `branding`) compiles lucent-worker's
    generated script, which the target property `LUCENT_RESOURCE_SCRIPT` names, with its icon copied
    under a folder named with an o-umlaut and a Chinese character.
25. **The About logo's plate lines up with the text column** (decision 8; BRD-11). The image stood at
    the column's left edge, so the plate began the image's margin (decisions 6 and 21: the content box
    keeps round(size / 16) pixels from each edge) right of the text below it, 5 device pixels at
    125 %. The image now starts that much further left, in whole device pixels (`Layout.leftMargin`
    -round(64 x ratio / 16) / ratio); the margin lies left of the column, where the dialog's Flickable
    clips it. The plate's low contrast on the dialog's surface (about 1.00:1 against `#232323`) is the
    artwork's design, as on a dark taskbar ("Consequences"), and is kept: a ring or a lighter tile
    behind the logo would be the owner's choice.
26. **The About logo is tested at fractional scales** (decision 14; BRD-08). The test ran only at the
    offscreen screen's ratio 1, where every whole logical position is a whole device position, and its
    grab cannot show a logo half a device pixel off (the tests' software renderer draws an image on
    the nearest whole pixel). `QmlShellTest.TheWindowIconAndTheAboutLogoAreLucentsLogoAtEveryScale`
    now works at the window's ratio: the logo's scene position is on whole device pixels, the content
    box's left edge (alpha 128 and more) is the text column's, and the grab, in device pixels, is the
    image of exactly 64 x ratio pixels composited on the dialog's surface within 3 levels, edges
    included. CTest runs it again with `QT_SCALE_FACTOR` 1.25 and 1.75 (`..._at_125_percent`,
    `..._at_175_percent`, labels `ui;branding`), with `--gtest_fail_if_no_test_selected`, so a renamed
    test fails them.

### Evidence (2026-10-15)

- **The review's reproductions** (copies of the asset, with the generator as committed in this
  change's first version): a manifest entry `../../victim.txt` and `-Version 1.0.1` deleted a file
  outside the copy; a hidden extra PNG and a hidden system `desktop.ini` passed `-Check`; a master with
  a `cICP` chunk (Display P3, and BT.2020 with PQ) was accepted; after a deleted derivative the advised
  `-Version` was refused, and the run without it restored the file but said "nothing changed"; a
  replaced logo (912,584 bytes) failed `lucent_branding_tests` on the hard-coded master size.
- **`lucent_branding_tests`** (`Test-Branding.ps1`): 259 checks (230 before). New: a manifest naming
  `../victim.txt` is refused by the generator, nothing is written or deleted and the file outside
  survives, and the path rule refuses traversal, drives, backslashes and absolute paths; a hidden PNG
  and a hidden system `desktop.ini` are found by `-Check` and by packaging's reader; a deleted or
  damaged derivative is one problem, the advice is the generator without `-Version`, which restores it
  and keeps the version; an unlisted file's advice is to delete it; a licence recorded by hand in
  `provenance.json` is found, the advice is a PATCH version, and generating with it passes; masters
  with `cICP`, `mDCV`, `cLLI` or an unknown chunk are refused and one with a `tEXt` chunk is accepted;
  the master's size and the icon's sizes come from the asset.
- **The asset.** Generated again as 1.0.1: the generator wrote `branding.json` (the method text) and
  `manifest.json` only; `Update-Branding.ps1 -Check` passes. `lucent_setup_branding_tests` (20 checks,
  ISCC 6.7.3) and the packaging run of `build\wl-fix-scripts-rel` (`Lucent.exe`, `lucent-worker.exe`:
  `IDI_ICON1`, the ten images byte for byte) read branding 1.0.1.
- **Items 24 to 26.** rc.exe 10.0.26100 with the real `lucent.ico` under a folder named with an
  o-umlaut and a Chinese character: the generated script without the pragma fails with RC2135 (exit
  1); with it RC writes 199,736 bytes of resources. The About test, with the dialog's position rounded
  in logical pixels and the logo at the column's edge as before, fails at ratio 1 (the plate 4 device
  pixels right of the column), at 125 % (the logo at x 237.5 device pixels, the plate 5.5 right of the
  column) and at 175 %; now it passes there and with `QT_SCALE_FACTOR` 1.5, 2.25, 2.5 and 3.
  `Lucent.exe --show-about --screenshot` from the `dev` build tree with a temporary `--data-root` on
  the 125 % screen (a 2160 x 1350 grab): the logo is the 80-pixel image drawn 1:1 (every pixel within
  1 level of it composited on the dialog's surface) and its plate starts at x 718, the text column's
  leftmost glyph pixel; in `build\wl-brand-evidence\about-dialog.png` it started at x 543 against 538.
  In every `app-icon` image the About logo uses (64 to 192 pixels) the left margin is fully
  transparent (item 21's rim reaches only into the top and bottom margins), so the clip removes
  nothing.
- **The release review's integration** (the branch with every review fix, uncommitted; six jobs).
  Fresh `dev-core`: 2,028 tests, 1,965 passed, 57 fixture tests skipped by their tier, 6 disabled.
  Fresh `dev`: 2,311 tests, 2,247 passed, 57 skipped, 6 disabled, 1 failed:
  `AutoServiceTest.AutoCreatesAnEditableRevisionWithItsAnalysisRecord` read the run's completion
  event as soon as `wait()` returned, before the executor thread had delivered it (`auto_service.hpp`
  promises no order between the two; under load it failed 1 run in 60); the test now waits for the
  event (150 runs of 150 passed under the same load), and both trees ran every test again without a
  failure. `tools\package.ps1 -Fresh -RunTests -UpdateChannel none -VersionRehearsal`: 2,311 tests,
  2,248 passed, 57 skipped, 6 disabled; the audit, the smoke run and the builder-path scan passed;
  `Lucent.exe` and `lucent-worker.exe` carry `IDI_ICON1` and Setup (44,408,407 bytes) `MAINICON`,
  each its only icon group, with the icon's ten images byte for byte.
  `Test-InstalledPackage.ps1 -RuntimeOutcomes -NewerInstaller` with the version rehearsal: 68 of 68
  checks; `Test-SourcePackage.ps1` on the package manifest: 139 checks. `lucent_bench_harness_tests`,
  which failed in this phase's `dev` run of 2026-10-14, passed in every run; its cause is fixed (ADR
  0021, item 18). The About screenshots of the `dev` build at 125 % and, with `QT_SCALE_FACTOR`, at
  175 and 225 %, and of the package at 125 %, show the image of 64 x ratio pixels 1:1 (within 1
  level), its plate at the text column's leftmost glyph pixel; the regenerated contact sheet differs
  from the first one only in the version in its title (`build\wl-brand-evidence\2026-10-15`, with a
  README).
