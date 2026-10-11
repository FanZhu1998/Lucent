; Lucent per-user installer: Lucent-Setup-<version>-x64.exe (Inno Setup 6.3 or later).
;
; Compiled by tools\package.ps1 from the audited installed layout; see
; docs/adr/0009-windows-packaging.md. The script expects these defines (ISCC /D):
;
;   AppVersion                  product version from CMake, for example 0.1.0
;   AppVersionQuad              the same as four numbers, for example 0.1.0.0
;   StagingDir                  the installed layout (cmake --install, licences, SBOM)
;   VCRedistFile                Microsoft's vc_redist.x64.exe (Authenticode-checked by package.ps1)
;   VCRedistMajor, VCRedistMinor, VCRedistBuild
;                               its version, for example 14, 44, 35211: the oldest runtime accepted
;   VCRuntimeDlls               comma-separated runtime DLLs Lucent imports (from the audit)
;   AssociationsInclude         generated file with the "Open with Lucent" registry entries
;   LucentSign                  optional: sign Setup and the uninstaller with the "lucent" sign tool,
;                               which package.ps1 defines with ISCC /S when signing is configured.
;                               Inno Setup signs the uninstaller inside Setup; a signed uninstaller
;                               keeps its messages in unins000.msg beside unins000.exe, which
;                               installer\tests\Test-InstalledPackage.ps1 -SignerThumbprint checks
;   VersionRehearsalOf          optional (tools\package.ps1 -VersionRehearsal, W12): this Setup is a
;                               version rehearsal of the package version given here: AppVersion is
;                               the next patch version, with the same files, so that the installer
;                               test can upgrade to it and see the real Setup refuse to go back. It
;                               says so in its name and version information; never published.
;
; Policies (ADR 0009):
; - Per user, no elevation: PrivilegesRequired=lowest installs into %LOCALAPPDATA%\Programs\Lucent.
;   Only Microsoft's runtime installer is elevated, and only when the installed runtime is older.
; - x64 Windows only (x64os): Windows on Arm is not supported until its runtime path is tested.
; - "Open with Lucent" for RAW files is optional and never changes what opens a file: Lucent is
;   listed for every RAW type (Applications\Lucent.exe\SupportedTypes) and added to a type's
;   OpenWithProgids only when that type already has a default program.
; - Upgrades delete only the files that the previous version's Lucent-files.txt lists and the new
;   version no longer installs, after Restart Manager has closed a running Lucent.
; - Uninstall removes the program, its shortcuts and registry entries, and keeps the user's library
;   (catalog), settings and caches in %LOCALAPPDATA%\Lucent and HKCU\Software\Lucent.
; - Never over a newer Lucent (W12 review SEC-04, part 2; ADR 0009 amendment): when this user's
;   installation (its uninstall registration's DisplayVersion) is newer than this Setup's version by
;   Semantic Versioning precedence, or its version cannot be read, Setup stops at InitializeSetup,
;   before anything is changed or the runtime installer runs: a message box (suppressed by
;   /SUPPRESSMSGBOXES), the log line "Downgrade refused: ...", and exit code 1 ("Setup failed to
;   initialize"). The same version is repaired and an older one upgraded. To go back to an older
;   version the user uninstalls first (the library is kept).

#if VER < EncodeVer(6, 3, 0)
  #error Inno Setup 6.3 or later is required.
#endif
#ifndef AppVersion
  #error AppVersion is not defined: build the installer with tools\package.ps1.
#endif
#ifndef AppVersionQuad
  #error AppVersionQuad is not defined: build the installer with tools\package.ps1.
#endif
#ifndef StagingDir
  #error StagingDir is not defined: build the installer with tools\package.ps1.
#endif
#ifndef VCRedistFile
  #error VCRedistFile is not defined: build the installer with tools\package.ps1.
#endif
#ifndef VCRedistBuild
  #error VCRedistMajor/VCRedistMinor/VCRedistBuild are not defined: build the installer with tools\package.ps1.
#endif
#ifndef VCRuntimeDlls
  #error VCRuntimeDlls is not defined: build the installer with tools\package.ps1.
#endif
#ifndef AssociationsInclude
  #error AssociationsInclude is not defined: build the installer with tools\package.ps1.
#endif
#if AppVersionQuad != AppVersion + ".0"
  #error AppVersionQuad must be AppVersion followed by .0: Lucent's versions are MAJOR.MINOR.PATCH.
#endif

#define AppName "Lucent"
#define AppExe "Lucent.exe"
#define AppUrl "https://github.com/FanZhu1998/Lucent"
; Identifies Lucent for upgrades and uninstall. Never change it.
#define AppGuid "ADBA4913-BBBD-42D8-90EF-0E9C80412232"
#define ProgId "Lucent.RawPhoto"
; Written into the staged folder by tools\package.ps1 (Write-InstalledFileList): every file Setup
; installs, relative to the installation folder.
#define InstalledFileList "Lucent-files.txt"
#define InstalledFileListHeader "; Lucent installed files, format 1"

[Setup]
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
#ifdef VersionRehearsalOf
AppVerName={#AppName} {#AppVersion} (version rehearsal of {#VersionRehearsalOf}, not for distribution)
#else
AppVerName={#AppName} {#AppVersion}
#endif
AppPublisher={#AppName}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppCopyright=Copyright (c) 2026 Fan Zhu. MIT License.
VersionInfoVersion={#AppVersionQuad}
VersionInfoProductName={#AppName}
#ifdef VersionRehearsalOf
VersionInfoDescription={#AppName} Setup, version rehearsal of {#VersionRehearsalOf}, not for distribution
#else
VersionInfoDescription={#AppName} Setup
#endif
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.17763
OutputDir=output
OutputBaseFilename=Lucent-Setup-{#AppVersion}-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=no
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
#ifdef LucentSign
SignTool=lucent
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
ConfirmUninstall=Are you sure you want to remove %1?%n%nYour photo library (the catalog and its backups), your settings and Lucent's caches are kept. They are stored in AppData\Local\Lucent and in the registry under Software\Lucent. Your photographs and exported images are never touched.
UninstalledAll=%1 was removed from your computer.%n%nYour photo library, settings and caches were kept in AppData\Local\Lucent. You can delete that folder if you no longer need them; your photographs are not stored there.

[CustomMessages]
OpenWithTask=List Lucent under "Open with" for camera RAW files (what opens a file when you double-click it is not changed)
FileTypesGroup=File types:
VCRuntimeTitle=Installing prerequisites
VCRuntimeDescription=Lucent needs the Microsoft Visual C++ Runtime.
VCRuntimeInstalling=Installing the Microsoft Visual C++ Runtime (x64). Windows may ask for permission...
VCRuntimeCancelled=Lucent needs the Microsoft Visual C++ Runtime (x64) %1 or later, and its installation was cancelled, so Setup cannot continue.%n%nRun Setup again and allow the runtime installation (an administrator may need to approve it), or install the "Microsoft Visual C++ Redistributable (x64)" from Microsoft first.
VCRuntimeBusy=Another installation is in progress, so the Microsoft Visual C++ Runtime could not be installed. Finish or restart that installation, then run Setup again.
VCRuntimeFailed=The Microsoft Visual C++ Runtime (x64) could not be installed (%1), so Setup cannot continue.%n%nInstall the "Microsoft Visual C++ Redistributable (x64)" %2 or later from Microsoft, then run Setup again.
VCRuntimeStillMissing=The Microsoft Visual C++ Runtime installer finished, but the runtime is still missing or older than %1, so Setup cannot continue.%n%nRestart Windows and run Setup again. If this message returns, repair "Microsoft Visual C++ Redistributable (x64)" in Settings > Apps, or install it from Microsoft.
NewerVersionInstalled=Lucent %1 is installed, which is newer than Lucent %2, the version this Setup installs. Setup does not replace a newer Lucent with an older one: the newer version may have upgraded your photo library to a format an older Lucent can open only read-only, or not at all. Nothing was changed.%n%nTo keep Lucent %1, close this message. To go back to Lucent %2, first uninstall Lucent in Settings > Apps (your photo library, settings and photographs are kept), then run this Setup again.
UnknownVersionInstalled=Lucent is installed, but Setup cannot read its version ("%1"), so it does not replace it: it might be newer than Lucent %2, the version this Setup installs. Nothing was changed.%n%nUninstall Lucent first in Settings > Apps (your photo library, settings and photographs are kept), then run this Setup again.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "openwith"; Description: "{cm:OpenWithTask}"; GroupDescription: "{cm:FileTypesGroup}"; Flags: unchecked

[Files]
; Prerequisite first, extracted only when needed (PrepareToInstall). The file is named vc_redist.x64.exe.
Source: "{#VCRedistFile}"; Flags: dontcopy noencryption
; The staged folder includes {#InstalledFileList}, the list the next version's upgrade reads.
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Develop camera RAW photographs"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Comment: "Develop camera RAW photographs"; Tasks: desktopicon

[Registry]
; "Open with Lucent" (task openwith), per user under HKCU\Software\Classes: a ProgID of Lucent's own,
; Applications\Lucent.exe with SupportedTypes listing every RAW extension (Lucent appears under
; "Open with" for them), and an OpenWithProgids value only for extensions that already have a
; default program (ExtensionHasDefaultProgram), where it cannot change what a double-click opens.
; Uninstall deletes Lucent's keys and values only.
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: ""; ValueData: "Camera RAW photo"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"",0"; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: openwith
#include AssociationsInclude

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent; Check: LaunchAllowed

[Code]
const
  VCRuntimeKey = 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  RequiredMajor = {#VCRedistMajor};
  RequiredMinor = {#VCRedistMinor};
  RequiredBuild = {#VCRedistBuild};
  RuntimeDlls = '{#VCRuntimeDlls}';
  FileExtsKey = 'Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\';
  TestHookStartFailed = 'start-failed:';
  RC_CANCELLED = 1223;
  RC_USER_EXIT = 1602;
  RC_ALREADY_RUNNING = 1618;
  RC_NEWER_INSTALLED = 1638;
  RC_REBOOT_INITIATED = 1641;
  RC_REBOOT_REQUIRED = 3010;

var
  VCRuntimeRestartNeeded: Boolean;
  VCRuntimePage: TOutputProgressWizardPage;
  PreviousFiles: TStringList;

function RequiredVersionText: String;
begin
  Result := Format('%d.%d.%d', [RequiredMajor, RequiredMinor, RequiredBuild]);
end;

function VersionAtLeast(Major, Minor, Build: Cardinal): Boolean;
begin
  if Major <> RequiredMajor then
    Result := Major > RequiredMajor
  else if Minor <> RequiredMinor then
    Result := Minor > RequiredMinor
  else
    Result := Build >= RequiredBuild;
end;

// Microsoft's documented detection key: Installed = 1 and Major.Minor.Bld at least the bundled
// redistributable's version. Both registry views are read; newer redistributables have been seen to
// update only one of them.
function RegistryViewHasRuntime(RootKey: Integer; const ViewName: String): Boolean;
var
  Installed, Major, Minor, Build: Cardinal;
begin
  Result := False;
  if not RegQueryDWordValue(RootKey, VCRuntimeKey, 'Installed', Installed) then
  begin
    Log('VC++ runtime: no ' + ViewName + ' registration');
    exit;
  end;
  if (Installed <> 1) or not RegQueryDWordValue(RootKey, VCRuntimeKey, 'Major', Major) or
     not RegQueryDWordValue(RootKey, VCRuntimeKey, 'Minor', Minor) or
     not RegQueryDWordValue(RootKey, VCRuntimeKey, 'Bld', Build) then
  begin
    Log('VC++ runtime: incomplete ' + ViewName + ' registration');
    exit;
  end;
  Result := VersionAtLeast(Major, Minor, Build);
  // (No [Code] line may start with a bracket: Inno Setup would read it as a section tag.)
  Log(Format('VC++ runtime: %s registration %d.%d.%d (needs %s): %s', [ViewName, Major, Minor, Build,
    RequiredVersionText, IntToStr(Ord(Result))]));
end;

// The runtime DLLs Lucent imports must exist in the 64-bit System32 folder (the sysnative constant:
// Setup is a 32-bit process) with at least the bundled version.
function RuntimeFilesPresent: Boolean;
var
  Names: TStringList;
  I: Integer;
  Path: String;
  Major, Minor, Revision, Build: Word;
begin
  Result := True;
  Names := TStringList.Create;
  try
    Names.CommaText := RuntimeDlls;
    for I := 0 to Names.Count - 1 do
    begin
      Path := ExpandConstant('{sysnative}\') + Names[I];
      if not GetVersionComponents(Path, Major, Minor, Revision, Build) then
      begin
        Log('VC++ runtime: ' + Path + ' is missing');
        Result := False;
      end
      else if not VersionAtLeast(Major, Minor, Revision) then
      begin
        Log(Format('VC++ runtime: %s is %d.%d.%d', [Path, Major, Minor, Revision]));
        Result := False;
      end;
    end;
  finally
    Names.Free;
  end;
end;

function VCRuntimeSatisfied: Boolean;
var
  Registered: Boolean;
begin
  Registered := RegistryViewHasRuntime(HKLM64, '64-bit');
  if not Registered then
    Registered := RegistryViewHasRuntime(HKLM32, '32-bit');
  Result := Registered and RuntimeFilesPresent;
end;

// --- Never over a newer Lucent (W12 review SEC-04, part 2) ---------------------------------------
//
// Lucent's versions follow Semantic Versioning 2.0.0 (ADR 0022): MAJOR.MINOR.PATCH, optionally
// -PRERELEASE (dot-separated identifiers) and +BUILD (ignored for precedence).

function IsDigitText(const S: String): Boolean;
var
  I: Integer;
begin
  Result := Length(S) > 0;
  for I := 1 to Length(S) do
    if (S[I] < '0') or (S[I] > '9') then
      Result := False;
end;

// A pre-release identifier: [0-9A-Za-z-]+, and a numeric one without a leading zero.
function IsIdentifierText(const S: String): Boolean;
var
  I: Integer;
begin
  Result := Length(S) > 0;
  for I := 1 to Length(S) do
    if not (((S[I] >= '0') and (S[I] <= '9')) or ((S[I] >= 'A') and (S[I] <= 'Z')) or
            ((S[I] >= 'a') and (S[I] <= 'z')) or (S[I] = '-')) then
      Result := False;
  if Result and IsDigitText(S) and (Length(S) > 1) and (S[1] = '0') then
    Result := False;
end;

// The dot-separated parts of S (an empty part stays, so '1..2' has three).
function SplitDots(const S: String): TArrayOfString;
var
  Rest: String;
  P, N: Integer;
begin
  Rest := S;
  N := 0;
  repeat
    P := Pos('.', Rest);
    N := N + 1;
    SetArrayLength(Result, N);
    if P > 0 then
    begin
      Result[N - 1] := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end
    else
      Result[N - 1] := Rest;
  until P = 0;
end;

// Core receives MAJOR, MINOR and PATCH, Pre the pre-release identifiers (none: empty). False when
// Text is not a Semantic Version.
function ParseVersion(const Text: String; var Core, Pre: TArrayOfString): Boolean;
var
  Version, PreText: String;
  P, I: Integer;
begin
  Result := False;
  Version := Text;
  P := Pos('+', Version);
  if P > 0 then
    Version := Copy(Version, 1, P - 1);
  PreText := '';
  P := Pos('-', Version);
  if P > 0 then
  begin
    PreText := Copy(Version, P + 1, Length(Version));
    Version := Copy(Version, 1, P - 1);
    if PreText = '' then
      exit;
  end;
  Core := SplitDots(Version);
  if GetArrayLength(Core) <> 3 then
    exit;
  for I := 0 to 2 do
    if not IsDigitText(Core[I]) or ((Length(Core[I]) > 1) and (Core[I][1] = '0')) then
      exit;
  SetArrayLength(Pre, 0);
  if PreText <> '' then
  begin
    Pre := SplitDots(PreText);
    for I := 0 to GetArrayLength(Pre) - 1 do
      if not IsIdentifierText(Pre[I]) then
        exit;
  end;
  Result := True;
end;

function Sign(Value: Integer): Integer;
begin
  if Value < 0 then
    Result := -1
  else if Value > 0 then
    Result := 1
  else
    Result := 0;
end;

// Numbers of any length (no leading zeros): -1, 0 or 1.
function CompareDigits(const A, B: String): Integer;
begin
  if Length(A) <> Length(B) then
    Result := Sign(Length(A) - Length(B))
  else
    Result := Sign(CompareStr(A, B));
end;

// Semantic Versioning precedence (section 11): -1, 0 or 1.
function CompareVersions(const ACore, APre, BCore, BPre: TArrayOfString): Integer;
var
  I, Count: Integer;
begin
  for I := 0 to 2 do
  begin
    Result := CompareDigits(ACore[I], BCore[I]);
    if Result <> 0 then
      exit;
  end;
  // A release ranks above its pre-releases.
  if (GetArrayLength(APre) = 0) or (GetArrayLength(BPre) = 0) then
  begin
    Result := Sign(GetArrayLength(BPre) - GetArrayLength(APre));
    exit;
  end;
  Count := GetArrayLength(APre);
  if GetArrayLength(BPre) < Count then
    Count := GetArrayLength(BPre);
  for I := 0 to Count - 1 do
  begin
    if IsDigitText(APre[I]) and IsDigitText(BPre[I]) then
      Result := CompareDigits(APre[I], BPre[I])
    else if IsDigitText(APre[I]) then
      Result := -1
    else if IsDigitText(BPre[I]) then
      Result := 1
    else
      Result := Sign(CompareStr(APre[I], BPre[I]));
    if Result <> 0 then
      exit;
  end;
  Result := Sign(GetArrayLength(APre) - GetArrayLength(BPre));
end;

// Where Inno Setup registers this user's installation of Lucent (HKA: HKCU, Setup never runs in
// administrative install mode).
function UninstallRegistrationKey: String;
begin
  Result := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + '{#AppGuid}' + '}_is1';
end;

// Runs before anything else: Setup stops (exit code 1) when the installed Lucent is newer than this
// Setup's, or its version cannot be read (see the policy at the top).
// Test hook: /LucentTestCompareVersions=<a>,<b>;<c>,<d>;... logs how this Setup orders each pair
// ("Version comparison: a < b", "=", ">", or "invalid") and stops Setup (exit code 1) without
// reading or changing anything else, so installer\tests\Test-InstalledPackage.ps1 can check the
// precedence rules above on the compiled Setup. It can only stop Setup, never let it install.
procedure LogVersionComparisons(const Pairs: String);
var
  Rest, Pair, A, B, Relation: String;
  P: Integer;
  ACore, APre, BCore, BPre: TArrayOfString;
begin
  Rest := Pairs;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P > 0 then
    begin
      Pair := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end
    else
    begin
      Pair := Rest;
      Rest := '';
    end;
    P := Pos(',', Pair);
    if P = 0 then
      continue;
    A := Copy(Pair, 1, P - 1);
    B := Copy(Pair, P + 1, Length(Pair));
    if not ParseVersion(A, ACore, APre) or not ParseVersion(B, BCore, BPre) then
      Relation := 'invalid'
    else
      case CompareVersions(ACore, APre, BCore, BPre) of
        -1: Relation := '<';
        0: Relation := '=';
      else
        Relation := '>';
      end;
    Log('Version comparison: ' + A + ' ' + Relation + ' ' + B);
  end;
end;

function InitializeSetup: Boolean;
var
  Installed, Probe: String;
  InstalledCore, InstalledPre, ThisCore, ThisPre: TArrayOfString;
begin
  Result := True;
  Probe := ExpandConstant('{param:LucentTestCompareVersions|}');
  if Probe <> '' then
  begin
    LogVersionComparisons(Probe);
    Log('Version comparison test hook: Setup stops');
    Result := False;
    exit;
  end;
  if not ParseVersion('{#AppVersion}', ThisCore, ThisPre) then
  begin
    Log('Downgrade check: this Setup''s version {#AppVersion} is not a Semantic Version; Setup stops');
    SuppressibleMsgBox('This Setup''s version ({#AppVersion}) is invalid, so it cannot check the installed Lucent. Setup stops.', mbCriticalError, MB_OK, IDOK);
    Result := False;
    exit;
  end;
  if not RegQueryStringValue(HKA, UninstallRegistrationKey, 'DisplayVersion', Installed) then
  begin
    Log('Downgrade check: Lucent is not installed for this user');
    exit;
  end;
  if not ParseVersion(Installed, InstalledCore, InstalledPre) then
  begin
    Log('Downgrade refused: the installed Lucent''s version "' + Installed + '" cannot be read; this Setup installs {#AppVersion}');
    SuppressibleMsgBox(FmtMessage(CustomMessage('UnknownVersionInstalled'), [Installed, '{#AppVersion}']), mbError, MB_OK, IDOK);
    Result := False;
    exit;
  end;
  if CompareVersions(InstalledCore, InstalledPre, ThisCore, ThisPre) > 0 then
  begin
    Log('Downgrade refused: Lucent ' + Installed + ' is installed, newer than {#AppVersion}, which this Setup installs');
    SuppressibleMsgBox(FmtMessage(CustomMessage('NewerVersionInstalled'), [Installed, '{#AppVersion}']), mbError, MB_OK, IDOK);
    Result := False;
    exit;
  end;
  if CompareVersions(InstalledCore, InstalledPre, ThisCore, ThisPre) = 0 then
    Log('Downgrade check: Lucent ' + Installed + ' is installed; this Setup repairs it')
  else
    Log('Downgrade check: Lucent ' + Installed + ' is installed; this Setup upgrades it to {#AppVersion}');
end;

procedure InitializeWizard;
begin
  VCRuntimePage := CreateOutputProgressPage(CustomMessage('VCRuntimeTitle'), CustomMessage('VCRuntimeDescription'));
end;

// Runs Microsoft's redistributable elevated. Returns True when it ran; ExitCode receives its exit
// code (or the Windows error when it could not be started, 1223 when the elevation prompt was
// declined).
//
// Test hook: /LucentTestVCRedistExitCode=<code> simulates the exit code <code>, and
// /LucentTestVCRedistExitCode=start-failed:<error> a redistributable that could not be started,
// without running or elevating anything. PrepareToInstall honours it only when the runtime is
// already sufficient, so it can never leave an installation that lacks the runtime.
function RunVCRedist(const Simulated: String; var ExitCode: Integer): Boolean;
begin
  if Simulated <> '' then
  begin
    if Pos(TestHookStartFailed, Simulated) = 1 then
    begin
      ExitCode := StrToIntDef(Copy(Simulated, Length(TestHookStartFailed) + 1, Length(Simulated)), -1);
      Log('VC++ runtime: test hook, simulated start failure ' + IntToStr(ExitCode));
      Result := False;
    end
    else
    begin
      ExitCode := StrToIntDef(Simulated, -1);
      Log('VC++ runtime: test hook, simulated exit code ' + IntToStr(ExitCode));
      Result := True;
    end;
    exit;
  end;
  ExtractTemporaryFile('vc_redist.x64.exe');
  VCRuntimePage.SetText(CustomMessage('VCRuntimeInstalling'), '');
  VCRuntimePage.Show;
  try
    Log('VC++ runtime: running vc_redist.x64.exe /install /quiet /norestart (elevated)');
    Result := ShellExec('runas', ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /quiet /norestart',
      '', SW_SHOW, ewWaitUntilTerminated, ExitCode);
    if Result then
      Log('VC++ runtime: vc_redist.x64.exe exit code ' + IntToStr(ExitCode))
    else
      Log('VC++ runtime: could not start vc_redist.x64.exe: ' + IntToStr(ExitCode) + ' ' + SysErrorMessage(ExitCode));
  finally
    VCRuntimePage.Hide;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Started, Satisfied: Boolean;
  ExitCode: Integer;
  Simulated: String;
begin
  Result := '';
  Simulated := ExpandConstant('{param:LucentTestVCRedistExitCode|}');
  Satisfied := VCRuntimeSatisfied;
  if (Simulated <> '') and not Satisfied then
  begin
    Log('VC++ runtime: test hook ignored because the runtime is not installed; running the real installer');
    Simulated := '';
  end;
  if (Simulated = '') and Satisfied then
  begin
    Log('VC++ runtime: present, the bundled redistributable is not needed');
    exit;
  end;

  Started := RunVCRedist(Simulated, ExitCode);
  if not Started then
  begin
    if ExitCode = RC_CANCELLED then
      Result := FmtMessage(CustomMessage('VCRuntimeCancelled'), [RequiredVersionText])
    else
      Result := FmtMessage(CustomMessage('VCRuntimeFailed'), [SysErrorMessage(ExitCode), RequiredVersionText]);
    Log('VC++ runtime: Setup stops: ' + Result);
    exit;
  end;

  case ExitCode of
    0, RC_NEWER_INSTALLED:
      begin
        if ExitCode = RC_NEWER_INSTALLED then
          Log('VC++ runtime: a newer runtime is already installed');
        if not VCRuntimeSatisfied then
          Result := FmtMessage(CustomMessage('VCRuntimeStillMissing'), [RequiredVersionText]);
      end;
    RC_REBOOT_REQUIRED, RC_REBOOT_INITIATED:
      begin
        Log('VC++ runtime: installed; Windows must restart before Lucent runs');
        VCRuntimeRestartNeeded := True;
      end;
    RC_USER_EXIT, RC_CANCELLED:
      Result := FmtMessage(CustomMessage('VCRuntimeCancelled'), [RequiredVersionText]);
    RC_ALREADY_RUNNING:
      Result := CustomMessage('VCRuntimeBusy');
  else
    Result := FmtMessage(CustomMessage('VCRuntimeFailed'), ['exit code ' + IntToStr(ExitCode), RequiredVersionText]);
  end;
  if Result <> '' then
    Log('VC++ runtime: Setup stops: ' + Result);
end;

function NeedRestart(): Boolean;
begin
  Result := VCRuntimeRestartNeeded;
end;

// "Launch Lucent" is offered only when the runtime is usable without a restart.
function LaunchAllowed: Boolean;
begin
  Result := not VCRuntimeRestartNeeded;
end;

// "Open with Lucent" adds Lucent to an extension's OpenWithProgids only when Windows already opens
// that extension with a default program: the user's choice (UserChoice, or UserChoiceLatest on
// newer Windows 11) or a registered default ProgID. That default keeps winning. Without one, an
// OpenWithProgids entry would change what a double-click does (Lucent would become the program, or
// Windows would start asking which one to use), so Lucent is listed through SupportedTypes only.
function ExtensionHasDefaultProgram(const Extension: String): Boolean;
var
  ProgId: String;
begin
  Result := False;
  if (RegQueryStringValue(HKCU, FileExtsKey + Extension + '\UserChoice', 'ProgId', ProgId) and (ProgId <> '')) or
     (RegQueryStringValue(HKCU, FileExtsKey + Extension + '\UserChoiceLatest\ProgId', 'ProgId', ProgId) and (ProgId <> '')) then
    Result := True
  else if RegQueryStringValue(HKCR, Extension, '', ProgId) and (ProgId <> '') and
          (CompareText(ProgId, '{#ProgId}') <> 0) and RegKeyExists(HKCR, ProgId) then
    Result := True;
  Log(Format('Open with Lucent: %s has a default program: %s', [Extension, IntToStr(Ord(Result))]));
end;

// Reads an installed-file list ({#InstalledFileList}): lower-case relative paths. Returns nil when
// the file is missing or is not such a list, or when any entry is not a plain relative path inside
// the folder (then nothing at all is deleted).
function LoadInstalledFileList(const FileName: String): TStringList;
var
  Lines: TArrayOfString;
  I: Integer;
  Entry: String;
begin
  Result := nil;
  if not FileExists(FileName) then
    exit;
  if not LoadStringsFromFile(FileName, Lines) or (GetArrayLength(Lines) = 0) or
     (Lines[0] <> '{#InstalledFileListHeader}') then
  begin
    Log('Upgrade: ' + FileName + ' is not a Lucent installed-file list; nothing is removed');
    exit;
  end;
  Result := TStringList.Create;
  for I := 1 to GetArrayLength(Lines) - 1 do
  begin
    Entry := Trim(Lines[I]);
    if (Entry = '') or (Entry[1] = ';') then
      continue;
    if (Pos(':', Entry) > 0) or (Pos('..', Entry) > 0) or (Pos('/', Entry) > 0) or (Entry[1] = '\') or
       (Pos('*', Entry) > 0) or (Pos('?', Entry) > 0) then
    begin
      Log('Upgrade: ' + FileName + ' lists an invalid path (' + Entry + '); nothing is removed');
      Result.Free;
      Result := nil;
      exit;
    end;
    Result.Add(Lowercase(Entry));
  end;
end;

// Upgrading in place: files that the previous version installed and this one no longer ships must
// not linger. The previous version's list is read before any file is copied (ssInstall); after the
// new files are in place (ssPostInstall, when Restart Manager has already closed a running Lucent),
// exactly the listed files missing from the new list are deleted, then the folders they leave
// empty. Nothing else in the folder is touched, so another program's files in a shared folder
// survive, and a folder without a list (a fresh installation) is never cleaned.
procedure RemoveFilesOfPreviousVersion(const AppDir: String);
var
  NewFiles, Folders: TStringList;
  I, Removed, Failed: Integer;
  Folder: String;
  Changed: Boolean;
begin
  NewFiles := LoadInstalledFileList(AppDir + '{#InstalledFileList}');
  if NewFiles = nil then
  begin
    Log('Upgrade: this version''s installed-file list is missing; nothing is removed');
    exit;
  end;
  Folders := TStringList.Create;
  Removed := 0;
  Failed := 0;
  try
    for I := 0 to PreviousFiles.Count - 1 do
    begin
      if NewFiles.IndexOf(PreviousFiles[I]) >= 0 then
        continue;
      if FileExists(AppDir + PreviousFiles[I]) then
      begin
        if DeleteFile(AppDir + PreviousFiles[I]) then
        begin
          Removed := Removed + 1;
          Log('Upgrade: removed ' + PreviousFiles[I]);
        end
        else
        begin
          Failed := Failed + 1;
          Log('Upgrade: could not remove ' + PreviousFiles[I]);
        end;
      end;
      Folder := ExtractFileDir(PreviousFiles[I]);
      while (Folder <> '') and (Folders.IndexOf(Folder) < 0) do
      begin
        Folders.Add(Folder);
        Folder := ExtractFileDir(Folder);
      end;
    end;
    // RemoveDir removes only empty folders; repeat until no more go (children before parents).
    repeat
      Changed := False;
      for I := 0 to Folders.Count - 1 do
        if DirExists(AppDir + Folders[I]) and RemoveDir(AppDir + Folders[I]) then
          Changed := True;
    until not Changed;
    Log(Format('Upgrade: removed %d files of the previous version that this version does not install; %d could not be removed', [Removed, Failed]));
  finally
    Folders.Free;
    NewFiles.Free;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  AppDir: String;
begin
  AppDir := AddBackslash(ExpandConstant('{app}'));
  if CurStep = ssInstall then
  begin
    PreviousFiles := LoadInstalledFileList(AppDir + '{#InstalledFileList}');
    if PreviousFiles <> nil then
      Log(Format('Upgrade: the previous version''s list names %d files in %s', [PreviousFiles.Count, AppDir]));
  end
  else if (CurStep = ssPostInstall) and (PreviousFiles <> nil) then
  begin
    RemoveFilesOfPreviousVersion(AppDir);
    PreviousFiles.Free;
    PreviousFiles := nil;
  end;
end;
