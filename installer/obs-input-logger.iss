; Inno Setup script — builds a double-click Windows installer for obs-input-logger.
;
; Install target (per-user, no admin):
;   %APPDATA%\obs-studio\plugins\obs-input-logger\bin\64bit\obs-input-logger.dll
;   %APPDATA%\obs-studio\plugins\obs-input-logger\data\locale\en-US.ini
;
; The installer also:
;   - Refuses to run while OBS is open (old DLL stays locked otherwise).
;   - Detects a stale copy of obs-input-logger.dll under Program Files\obs-studio
;     (left over from a prior manual install). Warns and offers to delete it,
;     which requires a UAC elevation prompt.

#ifexist "defines.isi"
  #include "defines.isi"
#endif

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#ifndef StagingDir
  #define StagingDir "release\staging"
#endif

[Setup]
AppId={{8C9ABB7A-18B3-46A7-8AB4-76883B9CDB79}
AppName=OBS Input Logger
AppVersion={#MyAppVersion}
AppPublisher=Dylan
AppPublisherURL=https://github.com/DylanMercor/obs-input-logger
DefaultDirName={userappdata}\obs-studio\plugins\obs-input-logger
DefaultGroupName=OBS Input Logger
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=obs-input-logger-{#MyAppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=OBS Input Logger {#MyAppVersion}
UninstallDisplayIcon={app}\bin\64bit\obs-input-logger.dll
; Close OBS automatically if it's running so our DLL isn't file-locked.
CloseApplications=force
RestartApplications=no

[Files]
Source: "{#StagingDir}\bin\64bit\obs-input-logger.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#StagingDir}\bin\64bit\obs-input-logger.pdb"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StagingDir}\data\*";                        DestDir: "{app}\data";      Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Clean up any pre-existing per-user install tree from earlier layouts.
Type: filesandordirs; Name: "{userappdata}\obs-studio\plugins\obs-input-logger\bin"
Type: filesandordirs; Name: "{userappdata}\obs-studio\plugins\obs-input-logger\data"
Type: filesandordirs; Name: "{userappdata}\obs-studio\plugins\obs-input-logger\obs-plugins"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
// Common locations where older manually-copied DLLs could hide and take load
// precedence over our per-user install. We scan these on install and warn.
function StaleSystemwideFound(): Boolean;
begin
  Result :=
    FileExists(ExpandConstant('{commonpf}\obs-studio\obs-plugins\64bit\obs-input-logger.dll')) or
    DirExists (ExpandConstant('{commonpf}\obs-studio\data\obs-plugins\obs-input-logger')) or
    FileExists(ExpandConstant('{commonpf32}\obs-studio\obs-plugins\64bit\obs-input-logger.dll')) or
    DirExists (ExpandConstant('{commonpf32}\obs-studio\data\obs-plugins\obs-input-logger'));
end;

procedure TryDeleteStaleSystemwide();
var
  ResultCode: Integer;
  Paths: array[0..3] of string;
  I: Integer;
  Cmd, Args: string;
begin
  Paths[0] := ExpandConstant('{commonpf}\obs-studio\obs-plugins\64bit\obs-input-logger.dll');
  Paths[1] := ExpandConstant('{commonpf}\obs-studio\data\obs-plugins\obs-input-logger');
  Paths[2] := ExpandConstant('{commonpf32}\obs-studio\obs-plugins\64bit\obs-input-logger.dll');
  Paths[3] := ExpandConstant('{commonpf32}\obs-studio\data\obs-plugins\obs-input-logger');

  // Build a batch of deletions and invoke one elevated cmd.exe so the user
  // only sees a single UAC prompt.
  Args := '/C ';
  for I := 0 to 3 do
  begin
    if FileExists(Paths[I]) then
      Args := Args + 'del /f /q "' + Paths[I] + '" & ';
    if DirExists(Paths[I]) then
      Args := Args + 'rmdir /s /q "' + Paths[I] + '" & ';
  end;
  Args := Args + 'exit /b 0';

  Cmd := ExpandConstant('{cmd}');
  if not ShellExec('runas', Cmd, Args, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    MsgBox('Could not remove the stale system-wide DLL. Please delete these manually:' + #13#10 +
           Paths[0] + #13#10 + Paths[1] + #13#10 + Paths[2] + #13#10 + Paths[3],
           mbInformation, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    if StaleSystemwideFound() then
    begin
      if MsgBox(
           'An older copy of obs-input-logger is installed under Program Files\obs-studio. ' +
           'OBS loads that one instead of the new per-user install, which is why you may be ' +
           'seeing old behavior.' + #13#10 + #13#10 +
           'Delete the stale copy now? (Windows will prompt for admin permission.)',
           mbConfirmation, MB_YESNO) = IDYES then
        TryDeleteStaleSystemwide();
    end;
  end;
end;

[Messages]
WelcomeLabel2=This will install [name/ver] into your personal OBS Studio plugins folder.%n%nAfter the installer finishes, start OBS and open the Tools menu — you'll see "Input Logger: Enabled". Each recording will produce a .inputlog.jsonl file next to the video.%n%nNo administrator password needed for the install itself.
