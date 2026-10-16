# Lucent's logo: the branding asset

This folder holds Lucent's logo: **one master** and the images **generated** from it for every place
the logo appears: the icon of `Lucent.exe`, `lucent-worker.exe`, Setup and its uninstaller, the
application's window icon and Help > About, Setup's wizard images, and this repository's README.
The decision behind it is [ADR 0025](../../docs/adr/0025-branding.md).

Rules:

- The master, `master/lucent-logo.png`, is the image the project owner supplied, committed byte for
  byte. It keeps its content credentials (see [Provenance](#provenance)).
- Every other image here is made by `tools\branding\Update-Branding.ps1` from the master and is
  **never edited by hand**: the build checks every image it uses, and the packaging every file,
  against `manifest.json`, and CTest (`lucent_branding_tests`) checks that each one is exactly what the
  generator makes from the master.
- Programs and scripts find the images by **role** (in `branding.json`), never by file name or size,
  so a new logo or a new size changes no code.

## The files

| File | Role | Sizes (px) | Used by |
|---|---|---|---|
| `master/lucent-logo.png` | master | 1254 x 1254 | the generator only |
| `provenance.json` | | | where the master comes from and on which terms it is used (written by hand) |
| `branding.json` | | | generated: every image with its role, pixel size and composition, and how they were made ("lucent.branding" version 1) |
| `manifest.json` | | | generated: every file but itself and this README, with its size and SHA-256 ("lucent.asset-manifest" version 1, the format of [ADR 0018](../../docs/adr/0018-camera-support-data.md)) |
| `windows/lucent.ico` | `windows-icon` | 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 | `Lucent.exe` and `lucent-worker.exe` as the icon resource `IDI_ICON1` (`cmake/LucentBranding.cmake`): Explorer, the taskbar, shortcuts, the uninstall entry and "Open with" show it, and Qt uses it for the window class; Setup's and the uninstaller's icon (`SetupIconFile`) |
| `app/lucent-<N>.png` | `app-icon` | 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 256 | compiled into `Lucent.exe` as the Qt resource folder `:/lucent/branding/app-icon`: the window icon (every size) and the About dialog's 64-pixel logo (`apps/lucent/src/branding.hpp`) |
| `installer/wizard-image-<W>x<H>.png` | `setup-wizard-image` | 202x386, 269x515, 336x643, 403x772, 430x824, 498x953, 534x1022 | Setup's `WizardImageFile`, on its Finished page (Inno Setup 6 disables the Welcome page by default, and `installer\lucent.iss` keeps it so) |
| `installer/wizard-small-image-<N>.png` | `setup-wizard-small-image` | 58, 77, 97, 116, 124, 143, 159 | Setup's `WizardSmallImageFile` (the header of the other pages) |
| `docs/lucent-logo.png` | `docs-logo` | 256 | the logo at the top of the repository's `README.md` |

The sizes are those the consumers ask for: the window's small and large icons (16 and 32 logical
pixels) and the About logo (64) at the display scales Windows offers from 100 to 300 %, the sizes
Windows and Inno Setup recommend for an icon, and the image areas Inno Setup 6.7.3 documents for
100 to 250 % (Setup picks the image that fits its scale). Nothing here is installed as a file: the
images are inside the executables and Setup.

## How the images are made

`Update-Branding.ps1` (the module `tools\branding\LucentBranding.psm1`, its image code in
`LucentBranding.cs`) renders every image from the master, deterministically:

1. **Validate the master.** An 8-bit RGBA PNG of 512 to 8192 pixels a side, sRGB, one image, its
   artwork roughly square (at most 1.25:1) and away from the edges. Its ancillary chunks may only be
   `sRGB`, `caBX` (content credentials) and metadata (`tEXt`, `zTXt`, `iTXt`, `pHYs`, `tIME`, `eXIf`,
   `bKGD`, `sPLT`): a colour profile, gamma or chromaticity chunk, PNG 3's `cICP` (Display P3, HDR) and
   its `mDCV` and `cLLI`, a transparency key, an animation, or any chunk not named here is refused.
2. **Alpha.** Alpha 1 and 2 are the image generator's noise and are cleared, then every group of
   touching non-transparent pixels in which no pixel reaches alpha 128 (specks, and a faint ring a few
   pixels outside the edge). The master's interior is not quite opaque (alpha 253 to 254): its
   1st-percentile alpha (253 here, at least 240 required) counts as opaque, so the artwork comes out
   **opaque** and its anti-aliased edges keep their ramp. A translucent artwork is refused: it would
   need a rule of its own.
3. **Placement.** The content box (the bounding box of alpha 128 and more; 1060 x 1065 px, about 12 px
   below the master's centre) is centred in each image's artwork square, its longer side spanning it.
   In icons the content box keeps round(size / 16) pixels from each side (1 px at 16, 2 px at 32,
   16 px at 256); the artwork's anti-aliased rim, below alpha 128, reaches about a pixel into that
   margin (alpha up to 26 at 256 px), so the margin is nearly, not exactly, transparent. The large
   wizard image shows the logo at half the panel's width, its middle at 38 % of the panel's height;
   the docs logo has a 4-pixel margin.
4. **Resampling.** Each pixel is the area average of the master under it (exact footprint coverage) in
   premultiplied linear light: no dark fringes, no colour shift, and sRGB decoded and encoded the same
   way everywhere.
5. **Encoding.** PNGs with IHDR, IDAT and IEND only (no metadata, no content credentials, no time) and
   one deterministic zlib stream: the same master gives the same bytes. The icon file holds 32-bit
   BMP images (BITMAPINFOHEADER, alpha, and an AND mask) below 256 px and a PNG at 256 px, the form
   the resource compiler, Explorer and Inno Setup all read.

`-Check` renders everything again and compares it with the committed files byte for byte.

## Replacing the logo

No code changes; this folder only:

1. Prepare the new master: an 8-bit RGBA PNG in sRGB without a colour profile, 512 to 8192 pixels a
   side, a transparent background, a roughly square artwork that does not touch the edges, opaque or
   nearly opaque inside. The artwork must be at least 267 pixels across (the largest rendering, in the
   534 x 1022 wizard image; the generator never enlarges), and a larger master gives smoother edges.
2. Record its provenance in `provenance.json`: its size and SHA-256
   (`Get-FileHash <file> -Algorithm SHA256`), what it shows, who supplied it and when, how it was
   made, and its licence terms in the form under [Licence](#licence) (or `null` while they are
   undecided: the generator and the tests accept that, `tools\package.ps1` does not). The generator
   refuses a master that the record does not describe.
3. Generate everything with a new branding version (Semantic Versioning: MINOR for a new master or new
   images, PATCH for the same master rendered differently or for a change of `provenance.json` or of
   the generated record's wording only, MAJOR when a role or a file is removed or renamed), and write a
   contact sheet:

   ```powershell
   powershell -NoProfile -File tools\branding\Update-Branding.ps1 -MasterFile C:\path\new-logo.png -Version 1.1.0 -ContactSheet build\branding-sheet.png
   ```

4. Look at the contact sheet: every image at 1:1 and magnified, on light, mid-grey and dark
   backgrounds, next to the master. Small sizes must stay legible, edges clean, the logo centred.
5. Build and test (`tools\build.ps1 -Preset dev -Test`): the configure step reads the new files, and
   `lucent_branding_tests`, `lucent_program_icon_tests`, `lucent_setup_branding_tests` and
   `lucent_ui_tests` check them; `tools\package.ps1` checks the packaged programs and Setup.
6. Commit the whole folder in one commit.

Adding a size or a target is one entry in `Get-BrandingRecipe` (`tools\branding\LucentBranding.psm1`)
and a new MINOR version; a new role also needs its consumer to ask for it. The tests take the master's
size and the icon's sizes from the asset itself (the icon must keep at least 16, 32, 48 and 256), so
neither a new logo nor a new size needs a test changed. A vector master (SVG) can
replace the PNG later without changing any consumer: the generator would rasterise it into the same
premultiplied, linear-light master it renders from (ADR 0025 describes how).

## Checking

```powershell
powershell -NoProfile -File tools\branding\Update-Branding.ps1 -Check
```

Exit code 0 when every image is what the generator makes from the master and every manifest entry is
right. The folder must hold exactly the listed files and this README, hidden or system files
included (`desktop.ini`, `Thumbs.db`: the C++ check sees them too). Otherwise it names each problem and
what to do about it: a missing or damaged image is written again by `Update-Branding.ps1` without
`-Version` (the version stays), an unlisted file is deleted, a changed `provenance.json` needs a higher
version, and a damaged manifest is restored from git (the generator refuses a manifest that names a
path outside this folder, and deletes nothing outside it). CTest runs it with more (`ctest -L
branding`: the asset, the built programs' icon resources, and a Setup compiled from
`installer\lucent.iss` with the icon and the wizard images); `tools\package.ps1` checks that the
packaged Lucent.exe, lucent-worker.exe and Setup carry the icon.

## Provenance

The master was supplied by Fan Zhu, Lucent's project owner, on 2026-10-11 as the application's logo.
It is a 1254 x 1254 pixel, 8-bit RGBA PNG (1,013,687 bytes, SHA-256
`106aaee47ba11568677fe96d509d903280be72227befc9d34edffa49437eab43`): a dark rounded square (about
`#18252B`) holding a two-tone "L", a teal vertical stroke (about `#ABD7CF`) and a pale foot (about
`#E3F1EC`) meeting at a 45-degree mitre, on a transparent background.

It was made with ChatGPT (OpenAI, image model gpt-image) on 2026-10-10. Its PNG carries a C2PA
content-credentials manifest in a `caBX` chunk (21,881 bytes), which the committed master keeps:
claim generator "OpenAI Media Service API" (C2PA 2.2); actions `c2pa.created` by ChatGPT (gpt-image)
at 2026-10-10T17:48:31Z with the digital source type `trainedAlgorithmicMedia`, `c2pa.converted` and
`c2pa.watermarked.unbound`; signed by "OpenAI Media Service" (OpenAI OpCo, LLC; certificate issued by
Trufo C2PA Claim Signing CA (2025)) with a time stamp of 2026-10-10T17:48:31Z. These facts were read
from the manifest's text; the signature was not validated, since Lucent's tools include no C2PA
validator. The generated images carry no metadata and no content credentials.

`provenance.json` records the same facts for the tools.

## Licence

Lucent's logo is **not covered by the MIT License** that covers Lucent's source code. Its terms,
decided by its owner, Fan Zhu, on 2026-10-15:

> Copyright (c) 2026 Fan Zhu. All rights reserved. Lucent's logo is not covered by the MIT License
> that covers Lucent's source code. Others may show it, unmodified, only to refer to Lucent. These
> terms do not limit the rights the GNU LGPL gives you to modify Lucent and its LGPL libraries for
> your own use and to reverse engineer Lucent for debugging such modifications.

They cover the master and every image in this folder, and the logo as it is built into
`Lucent.exe`, `lucent-worker.exe` and Setup and shown in this repository's README. The provenance
facts above stay as they are.

`provenance.json` records them (since branding 1.0.2), and `tools\package.ps1` takes them from there:
the installed `licenses\THIRD-PARTY-NOTICES.txt` states them at its top, in Lucent's own notice, and
the SBOM (`licenses\Lucent.spdx.json`) gives `Lucent.exe`, `lucent-worker.exe` and Lucent's package
the licence `MIT AND LicenseRef-Lucent-Logo`, with the text above for `LicenseRef-Lucent-Logo`.

`licence` in `provenance.json` is `null` while the owner has not decided, or an object of exactly:

| Member | What |
|---|---|
| `id` | the terms' SPDX identifier: `LicenseRef-` followed by letters, digits, `.` and `-` (`LicenseRef-Lucent-Logo`); the SBOM names the terms by it |
| `name` | a short name of the terms |
| `text` | the terms in full, as plain text |
| `decided` | the date of the owner's decision (YYYY-MM-DD) |

`licence_status` says, in words, who decided what and when. (The generator still accepts the terms
as text alone, the form `provenance.json` version 1 had before 2026-10-15, but packaging needs the
object, and refuses a logo whose terms are `null` or text alone.)

Changing the terms, or recording those of a new logo, is a change of the asset, made before a
release commit ([release checklist](../../docs/release/checklist.md), gate 8):

1. Set `licence` and `licence_status` in `provenance.json`, and state the terms in this section.
2. Run `powershell -NoProfile -File tools\branding\Update-Branding.ps1 -Version <the next PATCH
   version>`: `provenance.json` is listed in `manifest.json`, so the manifest records its new size and
   SHA-256 (the images do not change). `-Check` and `tools\package.ps1` refuse the folder until then.
3. Update the repository's README ("Licence") and the logo's row in the
   [licence ledger](../../docs/legal/third-party-notices.md) if they no longer describe the terms.
4. Commit, and start the release at gate 1 with that commit.
