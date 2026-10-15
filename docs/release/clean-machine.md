# The clean-machine gate

The blueprint's release gate "Offline installation" (section 10): *a clean supported Windows machine
with no Python, Qt SDK, Git, compiler, or development PATH installs and imports/edits/exports entirely
through the GUI while offline.* Lucent checks it in **Windows Sandbox**, a disposable Windows that
starts from the host's own Windows image with nothing installed: no Visual C++ runtime, no developer
tools, and, as the gate configures it, no network. Step 10 of the [release checklist](checklist.md)
runs it for every release; until it has passed, a release is not published.

**Status (2026-10-15):** Windows Sandbox is enabled on the reference machine. The first gate run, on
2026-10-11 with a release candidate, went through every step and failed. It showed three defects of
the harness and one of Setup, all fixed on 2026-10-15 and to be confirmed by the next run:
`gate.json` grew to 109 MB (W12 sandbox run SBX-1, below under step 6); the host read it while the
sandbox was still writing it and stopped (SBX-2); Setup made Lucent the program for RAW types whose
user choice names a program the sandbox does not hold (SBX-3,
[associations on a clean Windows](#associations-on-a-clean-windows)); and the scripts told the
operator to answer an elevation prompt that the sandbox never shows (SBX-4, step 2). The gate has
not passed yet.

## What the gate does

`tools\Test-CleanMachine.ps1` (on the host) prepares a gate folder and starts the sandbox:

| Folder | In the sandbox | Holds |
|---|---|---|
| `<gate>\input` | `C:\LucentGate\input`, **read-only** | `Lucent-Setup-<v>-x64.exe` and its package manifest, the version rehearsal Setup (`rehearsal\`, when the package has one), a few smoke fixtures (`fixtures\`), the gate's scripts (`scripts\`) |
| `<gate>\results` | `C:\LucentGate\results`, writable | what the gate records |
| `<gate>` itself | not mapped | `gate-request.json` (what the host asked for, with the host's Windows build and the Windows Sandbox version), the `.wsb`, and the host's verdict, `host-check.json` |

The configuration (`<gate>\lucent-clean-machine.wsb`) turns networking, the virtual GPU (Lucent then
draws with Windows' software renderer), the clipboard, printers and audio and video input off. Before
the sandbox starts, the host validates it against `installer\sandbox\wsb-configuration.xsd` (a
transcription of the elements and values Microsoft documents for `.wsb` files) and the gate's own
rules: networking disabled, exactly the read-only input and the writable results folder, a logon
command that runs the gate.

At logon the sandbox runs `installer\sandbox\Invoke-CleanMachineGate.ps1`, which:

1. **Records the machine** (`environment.json`): Windows version; whether the Visual C++ runtime is
   registered or its DLLs are in System32; `python`, `py`, `git`, `cl`, `link`, `cmake`, `ninja`,
   `vcpkg`, `qmake` or `windeployqt` on `PATH`; Qt variables; whether a network is available or a name
   resolves. *Clean* and *offline* are checks of their own. It also records its own token and
   Windows' UAC settings (`elevation`: the Administrators role, the integrity level, the elevation
   type, `EnableLUA`, `ConsentPromptBehaviorAdmin` and `-User`, `PromptOnSecureDesktop`,
   `FilterAdministratorToken`) and, from them, whether the runtime's installation can show a consent
   prompt at all.
2. **Installs silently** (`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER`). On a clean Windows,
   Setup first installs Microsoft's Visual C++ runtime, elevated. Windows Sandbox runs the logon
   command with an elevated administrator token (the first run's Setup log: "User privileges:
   Administrative"; the runtime installed in about four minutes without a prompt), so the per-user
   installation runs from an elevated process and **no elevation prompt is expected**: the check
   `install.exit` quotes Setup's "User privileges" line, and `gate.json` notes that the consent path
   was not exercised. The [manual steps](#the-manual-steps) check that path. Should a future Windows
   show a prompt in the sandbox window, answer Yes. Every installed file must be the package
   manifest's (SHA-256).
3. **Runs the installed `Lucent.exe`** with `PATH` reduced to Windows' own folders, no Qt variables and
   a throwaway `--data-root`, through its developer automation
   (`apps\lucent\src\developer_automation.hpp`):
   - run 1 imports the fixtures, opens the first, runs **Auto** on it, exports every photo as **JPEG**
     and saves a screenshot (`results\lucent-auto-jpeg.png`);
   - run 2 opens the same library and exports every photo as **16-bit TIFF**
     (`results\lucent-tiff.png`).

   Each run must exit with 0, load no Qt or QML module from anywhere else, report the installed camera
   support and lens databases as active, and produce one valid file per photo: a JPEG with its start
   and end markers, and a TIFF whose first image file directory says 16 bits for each of three samples
   (RGB) in one plane. Auto must finish with a summary; the fixtures must be unchanged.
4. **Uninstalls silently**: the program folder, the Start menu entry and the registration are gone,
   the library is kept.
5. **Runs the installer test**, `installer\tests\Test-InstalledPackage.ps1 -RuntimeOutcomes` (with
   `-NewerInstaller` when the package has a version rehearsal: install, same-version repair, upgrade,
   and Setup refusing to go back to the older version, also when the installed version cannot be
   read): associations, the upgrade's cleanup, Restart Manager, the runtime installer's outcomes, the
   uninstallation.
6. **Writes `results\gate.json`**, every check with a stable id and notes on what the run could not
   show (never failing), and shuts the sandbox down 15 seconds later (unless `-KeepOpen`).
   `results\progress.json` says which step runs meanwhile. Every result file is written complete
   under a temporary name (`<name>.partial`) and then renamed, so the host never reads a half-written
   file. `gate.json` holds plain data: Lucent's output is read as plain text lines (a line read with
   `Get-Content` carries PowerShell's provider properties, which the first run wrote out in full, 109
   MB of them), and a result above 1 MiB is refused with a failed `gate.json` that says why.

The host waits for the gate's first step (`results\progress.json`, at most `-StartTimeoutMinutes`,
default 5), then for `gate.json` (at most `-TimeoutMinutes`, default 60), reads it with retries while
the sandbox may still hold it, and waits for the sandbox to close. It then checks the result itself
(`Test-CleanMachineResult` in `installer\sandbox\LucentSandbox.psm1`) against `gate-request.json`, what
it prepared: every check present and passed, the machine clean and offline, one JPEG and one TIFF
recorded with its SHA-256 per photo, the photos it gave (by file name), the package's own Setup, and
the version rehearsal's upgrade and refused downgrade when it gave its Setup. It writes its verdict to
`<gate>\host-check.json`, outside the results folder the sandbox can write, whatever the outcome
(passed, failed, never started, timed out, no result or an unreadable one), with the host's facts, and
exits with 0 (passed), 1 (failed, timed out, never started, or another sandbox was running) or 2
(Windows Sandbox is not available).

## Associations on a clean Windows

"Open with Lucent" never changes what a double-click opens ([ADR 0009](../adr/0009-windows-packaging.md),
decision 6): Setup adds Lucent to a RAW type's `OpenWithProgids` only when Windows already opens that
type with a default program, and otherwise lists Lucent through `SupportedTypes` only ("Open with" >
"Choose another app"). Since 2026-10-15 "already opens it with a default program" means that Windows'
own query (`AssocQueryString`) resolves the type to a program and that this program is the user's
choice or the type's own default.

The first sandbox run showed why the query matters. The sandbox's profile names Photos as the user's
choice for 15 RAW types, but the sandbox does not hold Photos, so Windows opens those types with no
program. Setup counted that choice as a default program, added Lucent to the types'
`OpenWithProgids`, and Windows then opened them with Lucent, their only handler: the installer test
found `.arw`, `.cr2`, `.crw` and others changed from "no association" to Lucent. Now, on a clean
Windows, Lucent appears under "Choose another app" for every RAW type and becomes no type's program.
The installer test checks that what Windows opens each RAW type with (program, command and ProgID) is
the same after the installation, after each repair or upgrade and after the uninstallation, and that
Lucent writes no extension default value and no user choice.

## Enabling Windows Sandbox (once)

Windows Sandbox needs Windows 10 or 11 **Pro, Enterprise or Education** (not Home), x64,
**virtualisation enabled in the firmware** (AMD-V/SVM on the reference laptop's Ryzen, Intel VT-x
elsewhere), at least 4 GB of memory (8 GB or more recommended), 1 GB of free disk and two processor
cores. Enabling it is an administrator's change to Windows and needs a restart; the gate's scripts
never do it.

1. Check virtualisation: Task Manager > Performance > CPU shows **Virtualization: Enabled**. If it is
   disabled, enable SVM (AMD) or VT-x (Intel) in the computer's UEFI/BIOS setup first.
2. Turn the feature on, either:
   - **Settings > System > Optional features > More Windows features** (the classic *Turn Windows
     features on or off* dialog), tick **Windows Sandbox**, OK; or
   - in PowerShell **run as administrator**:
     `Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All`
3. **Restart Windows** when asked.
4. Check: `Test-Path "$env:SystemRoot\System32\WindowsSandbox.exe"` is `True`, and *Windows Sandbox*
   in the Start menu opens a sandbox window (close it again: everything in it is discarded).

If the sandbox does not start ("No hypervisor is present", "virtualisation disabled"), the firmware
setting is missing. Other virtualisation software (older VirtualBox or VMware releases) can conflict
with the Hyper-V platform the sandbox uses. On recent Windows 11 releases Windows Sandbox is also an
app that the Microsoft Store updates (`MicrosoftWindows.WindowsSandbox`, with the command line
`wsb.exe`); the host records its version in `gate-request.json` and `host-check.json`.

## Running the gate

From the repository root, with the release's package (checklist step 6):

```powershell
powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-<v>\package
```

It prints the gate folder (default `build\clean-machine-<time>`), starts the sandbox and shows the
gate's steps. Windows Sandbox runs one sandbox at a time, so the script refuses to start while one is
open (close it first; everything in it is discarded). No elevation prompt is expected. The first run
took about nine minutes in the sandbox, four of them installing the runtime; the gate's steps take
under three minutes on the reference laptop in the local rehearsal, the installer test included.

| Option | Use |
|---|---|
| `-Fixture <ids or files>` | Other photos (default: `dji-fc4382-dng`, `fujifilm-x-t4-raf-lossless`, `nikon-z-6-nef-lossless`: a DNG, an X-Trans file and a photo the lens database corrects; the first, by path, gets Auto) |
| `-KeepOpen` | Leave the sandbox open when the gate is done, to look around (close it yourself) |
| `-SkipInstallerTest` | Leave out step 5 |
| `-StartTimeoutMinutes` | How long the host waits for the gate's first step (default 5); a gate that has not started by then never will ([below](#when-the-gate-does-not-start)) |
| `-TimeoutMinutes`, `-MemoryMB` | The host's wait for the result (default 60) and the sandbox's memory (default 4096) |
| `-NoStart` | Prepare and validate only; start `lucent-clean-machine.wsb` yourself, then check with `-Check <gate folder>` |
| `-Check <gate folder>` | Check the result of a gate folder again |

**Pass:** the script prints `Clean-machine gate: PASSED` and exits with 0, and
`<gate>\host-check.json` says `"passed": true`. A `host-check.json` inside `results\` is not the
host's verdict (the sandbox can write there; the script warns about one). Keep the gate folder's
`results`, `gate-request.json` and `host-check.json` (with the date, the host's Windows build and the
Windows Sandbox version) in the release archive.

### When the gate does not start

When the gate writes no `results\progress.json` within `-StartTimeoutMinutes`, the host stops,
writes a failed `host-check.json` ("the gate never started") with its diagnosis and exits with 1.
Two causes are known:

- **Another sandbox was open.** Windows Sandbox runs one instance at a time. The script refuses to
  start while one runs (it asks `wsb list` where the command line exists and looks for the sandbox's
  processes), but a sandbox opened by hand afterwards, or one still closing, takes the place. Close
  it and run the gate again with a new gate folder.
- **The logon command did not run.** The Windows Sandbox app has been reported to open a sandbox with
  its mapped folders but without running the logon command (microsoft/Windows-Sandbox issue 125, app
  0.8.107.0 on Windows 11 build 26200; reported fixed in a later release, then reported again); the
  sandbox then sits at an idle desktop. On the reference machine the logon command ran (2026-10-11).
  Update the Windows Sandbox app (Microsoft Store) and run the gate again. If it still does not
  start, run the gate by hand in that sandbox: open Windows PowerShell in it and run the logon
  command of `<gate>\lucent-clean-machine.wsb`,

  ```powershell
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\LucentGate\input\scripts\Invoke-CleanMachineGate.ps1 -Root C:\LucentGate -Shutdown
  ```

  then, once the sandbox has closed, check the folder with `tools\Test-CleanMachine.ps1 -Check
  <gate folder>`, which writes the host's verdict again. The gate then runs with that PowerShell's
  token, which `environment.json` records; if the runtime's consent prompt appears, answer Yes. The
  issue also describes mapping a folder that holds a `.cmd` running the gate onto the sandbox user's
  Startup folder (`C:\Users\WDAGUtilityAccount\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup`).
  The gate's configuration rules (exactly two mapped folders) refuse that today: adopting it means
  changing `installer\sandbox\LucentSandbox.psm1` and its harness tests first.

### The manual steps

The automatic run uses Lucent's developer automation, and in Windows Sandbox it installs from an
elevated process: the ordinary path, a user who double-clicks Setup in a normal session and is asked
for consent to install the runtime, is not exercised there. The blueprint also asks for the graphical
user interface. The manual steps check both.

Steps 1 and 2 need a Windows that asks for consent: Windows 10 or 11 with UAC at its default
(*Notify me only when apps try to make changes to my computer*: `EnableLUA` 1,
`ConsentPromptBehaviorAdmin` 5) and without the Visual C++ runtime, offline, in a virtual machine
installed from Windows' own media (an image with Visual Studio already has the runtime) or on a spare
computer. Windows Sandbox does not qualify: it elevates its administrator without asking, as the
first run showed. Copy `Lucent-Setup-<v>-x64.exe` and one or two fixtures to that machine. Then:

1. Run `Lucent-Setup-<v>-x64.exe` by double-click and **decline** the elevation prompt: Setup says
   that the runtime's installation was cancelled so it cannot continue, and installs nothing (no Start
   menu entry, no `%LOCALAPPDATA%\Programs\Lucent`).
2. Run it again, accept the prompt, finish the wizard with its defaults and launch Lucent from its last
   page.
3. In Lucent: add a photo, open it, change the exposure, run Auto, and export it as JPEG to the
   Desktop. The export finishes and the file opens in Photos or Paint.
4. Uninstall Lucent from Settings > Apps: the program folder and the Start menu entry are gone,
   `%LOCALAPPDATA%\Lucent` remains.

Steps 3 and 4 may run in Windows Sandbox instead, after installing with the wizard there:

```powershell
powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-<v>\package -Manual
```

opens a sandbox with Explorer at `C:\LucentGate\input` (the photos are in `fixtures\`).

**Pass:** each step behaves as described, steps 1 and 2 on a machine that asked for consent. Record
that machine (Windows edition, version and build, virtual or physical, its UAC setting) with the
results.

## What is checked without Windows Sandbox

- `ctest -L installer` runs `lucent_clean_machine_harness_tests`
  (`installer\tests\Test-CleanMachineHarness.ps1`): the configuration the gate writes is valid against
  the schema and keeps the gate's rules, and configurations breaking either are refused; the scripts
  parse in Windows PowerShell 5.1 and are ASCII; every check the host requires is one the gate makes;
  the host's check refuses incomplete, failed, unclean, online, mismatched and foreign results, an
  empty or missing export list, an export without its file and SHA-256, other photos than those asked
  for, and a missing version rehearsal. The gate's helpers are tested on their own: its JSON is plain
  data (a line read with `Get-Content` is written as a string) and a result above 1 MiB is refused;
  the TIFF check accepts a 16-bit RGB TIFF in either byte order and refuses an 8-bit, RGBA, planar,
  truncated or BigTIFF file and a JPEG; the elevation record has its fields. The host reads a result
  that another process still holds or has not finished, parses `wsb list`, and writes
  `host-check.json` beside `gate-request.json`, also when the gate wrote no result.
- **The local rehearsal** runs the gate's logon script on the development machine against a real
  package, in a temporary, profile-like gate folder with a throwaway data root:

  ```powershell
  powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-<v>\package -LocalRehearsal
  ```

  Setup installs for the current user into `<gate>\profile\Programs\Lucent` (Lucent must not already
  be installed for that user; the script refuses otherwise), the two runs, the uninstallation and the
  installer test happen as in the sandbox, and the host checks the result. The development machine is
  neither clean nor offline: the rehearsal records that without failing, and it is never the gate's
  pass.

## What the gate does not show

- **The consent prompt.** Windows Sandbox runs the gate elevated; manual steps 1 and 2 run on a
  machine that asks.
- **A real graphics driver.** The virtual GPU is off; Lucent draws with Windows' software renderer.
  The reference machine and the installer test cover the hardware path.
- **Another Windows version.** The sandbox runs the host's Windows build. Windows 10 needs a Windows 10
  host (or a clean physical or virtual machine with the same scripts: `Invoke-CleanMachineGate.ps1`
  only needs Windows PowerShell 5.1).
- **A user's own clicks** beyond the manual steps above.
