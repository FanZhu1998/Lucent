# The clean-machine gate

The blueprint's release gate "Offline installation" (section 10): *a clean supported Windows machine
with no Python, Qt SDK, Git, compiler, or development PATH installs and imports/edits/exports entirely
through the GUI while offline.* Lucent checks it in **Windows Sandbox**, a disposable Windows that
starts from the host's own Windows image with nothing installed: no Visual C++ runtime, no developer
tools, and, as the gate configures it, no network. Step 10 of the [release checklist](checklist.md)
runs it for every release; until it has passed, a release is not published.

**Status (2026-10-10):** Windows Sandbox is not enabled on the reference machine yet. The harness is
complete and checked without it (below); the gate itself runs once the machine's owner has enabled
Windows Sandbox.

## What the gate does

`tools\Test-CleanMachine.ps1` (on the host) prepares a gate folder and starts the sandbox:

| Folder | In the sandbox | Holds |
|---|---|---|
| `<gate>\input` | `C:\LucentGate\input`, **read-only** | `Lucent-Setup-<v>-x64.exe` and its package manifest, the version rehearsal Setup (`rehearsal\`, when the package has one), a few smoke fixtures (`fixtures\`), the gate's scripts (`scripts\`) |
| `<gate>\results` | `C:\LucentGate\results`, writable | what the gate records |

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
   resolves. *Clean* and *offline* are checks of their own.
2. **Installs silently** (`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER`). On a clean Windows,
   Setup first installs Microsoft's Visual C++ runtime, which asks for elevation once: **answer Yes in
   the sandbox window**. Every installed file must be the package manifest's (SHA-256).
3. **Runs the installed `Lucent.exe`** with `PATH` reduced to Windows' own folders, no Qt variables and
   a throwaway `--data-root`, through its developer automation
   (`apps\lucent\src\developer_automation.hpp`):
   - run 1 imports the fixtures, opens the first, runs **Auto** on it, exports every photo as **JPEG**
     and saves a screenshot (`results\lucent-auto-jpeg.png`);
   - run 2 opens the same library and exports every photo as **16-bit TIFF**
     (`results\lucent-tiff.png`).

   Each run must exit with 0, load no Qt or QML module from anywhere else, report the installed camera
   support and lens databases as active, and produce one valid file per photo (JPEG and TIFF
   signatures checked); Auto must finish with a summary; the fixtures must be unchanged.
4. **Uninstalls silently**: the program folder, the Start menu entry and the registration are gone,
   the library is kept.
5. **Runs the installer test**, `installer\tests\Test-InstalledPackage.ps1 -RuntimeOutcomes` (with
   `-NewerInstaller` when the package has a version rehearsal: install, same-version repair, upgrade,
   and Setup refusing to go back to the older version): associations, the upgrade's cleanup, Restart
   Manager, the runtime installer's outcomes, the uninstallation.
6. **Writes `results\gate.json`**, every check with a stable id, and shuts the sandbox down 15 seconds
   later (unless `-KeepOpen`). `results\progress.json` says which step runs meanwhile.

The host waits for `gate.json` (at most `-TimeoutMinutes`, default 60), then checks it itself
(`Test-CleanMachineResult` in `installer\sandbox\LucentSandbox.psm1`) against `gate-request.json`,
what it prepared: every check present and passed, the machine clean and offline, one JPEG and one TIFF
per photo, the package's own Setup. It writes `results\host-check.json` and exits with 0 (passed), 1
(failed or timed out) or 2 (Windows Sandbox is not available).

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
with the Hyper-V platform the sandbox uses.

## Running the gate

From the repository root, with the release's package (checklist step 6):

```powershell
powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-<v>\package
```

It prints the gate folder (default `build\clean-machine-<time>`), starts the sandbox and shows the
gate's steps. Answer **Yes** when the sandbox asks for permission to install the Visual C++ runtime.
On the reference laptop the gate's steps took under three minutes in the local rehearsal, the
installer test included; allow longer in the sandbox, which renders in software and installs the
runtime first.

| Option | Use |
|---|---|
| `-Fixture <ids or files>` | Other photos (default: `dji-fc4382-dng`, `fujifilm-x-t4-raf-lossless`, `nikon-z-6-nef-lossless`: a DNG, an X-Trans file and a photo the lens database corrects; the first, by path, gets Auto) |
| `-KeepOpen` | Leave the sandbox open when the gate is done, to look around (close it yourself) |
| `-SkipInstallerTest` | Leave out step 5 |
| `-TimeoutMinutes`, `-MemoryMB` | The host's wait (default 60) and the sandbox's memory (default 4096) |
| `-NoStart` | Prepare and validate only; start `lucent-clean-machine.wsb` yourself, then check with `-Check <gate folder>` |
| `-Check <gate folder>` | Check the result of a gate folder again |

**Pass:** the script prints `Clean-machine gate: PASSED` and exits with 0, and
`results\host-check.json` says `"passed": true`. Keep the gate folder's `results` (with the date and
the host's Windows build) in the release archive.

### The manual steps

The automatic run uses Lucent's developer automation; the blueprint asks for the graphical user
interface as well. In a second sandbox (the first one has the runtime installed by now):

```powershell
powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-<v>\package -Manual
```

opens a sandbox with Explorer at `C:\LucentGate\input`. Then:

1. Run `Lucent-Setup-<v>-x64.exe` by double-click and **decline** the elevation prompt: Setup says
   that the runtime's installation was cancelled so it cannot continue, and installs nothing (no Start
   menu entry, no `%LOCALAPPDATA%\Programs\Lucent`).
2. Run it again, accept the prompt, finish the wizard with its defaults and launch Lucent from its last
   page.
3. In Lucent: add a photo from `C:\LucentGate\input\fixtures`, open it, change the exposure, run Auto,
   and export it as JPEG to the Desktop. The export finishes and the file opens in Photos or Paint.
4. Uninstall Lucent from Settings > Apps: the program folder and the Start menu entry are gone,
   `%LOCALAPPDATA%\Lucent` remains.

**Pass:** each step behaves as described.

## Without Windows Sandbox: what is checked today

- `ctest -L installer` runs `lucent_clean_machine_harness_tests`
  (`installer\tests\Test-CleanMachineHarness.ps1`): the configuration the gate writes is valid against
  the schema and keeps the gate's rules, and configurations breaking either are refused; the scripts
  parse in Windows PowerShell 5.1 and are ASCII; every check the host requires is one the gate makes;
  the host's check refuses incomplete, failed, unclean, online, mismatched and foreign results.
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

- **A real graphics driver.** The virtual GPU is off; Lucent draws with Windows' software renderer.
  The reference machine and the installer test cover the hardware path.
- **Another Windows version.** The sandbox runs the host's Windows build. Windows 10 needs a Windows 10
  host (or a clean physical or virtual machine with the same scripts: `Invoke-CleanMachineGate.ps1`
  only needs Windows PowerShell 5.1).
- **A user's own clicks** beyond the manual steps above.
