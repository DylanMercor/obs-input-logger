; Inno Setup script — Windows installer for obs-input-logger.
;
; Installs directly into the OBS installation folder (system-wide):
;   C:\Program Files\obs-studio\obs-plugins\64bit\obs-input-logger.dll
;   C:\Program Files\obs-studio\data\obs-plugins\obs-input-logger\locale\en-US.ini
;
; This is where OBS reliably scans on all setups we've seen. Requires
; admin elevation — installer triggers one UAC prompt at launch.

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
; System-wide install into the OBS install folder. Dir page lets advanced
; users point at a non-default OBS install (e.g. D:\obs-studio).
DefaultDirName={commonpf}\obs-studio
DisableDirPage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
AppendDefaultDirName=no
PrivilegesRequired=admin
OutputBaseFilename=obs-input-logger-{#MyAppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=OBS Input Logger {#MyAppVersion}
UninstallDisplayIcon={app}\obs-plugins\64bit\obs-input-logger.dll
; Close OBS automatically if it's running so our DLL isn't file-locked.
CloseApplications=force
RestartApplications=no

[Files]
Source: "{#StagingDir}\bin\64bit\obs-input-logger.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#StagingDir}\bin\64bit\obs-input-logger.pdb"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StagingDir}\data\*";                        DestDir: "{app}\data\obs-plugins\obs-input-logger"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Clean out any prior per-user install from 0.2.0–0.4.0 — those placed the DLL
; in %APPDATA%\obs-studio\plugins\obs-input-logger\ which OBS didn't find on
; the target system. Remove so there's only one copy of the plugin.
Type: filesandordirs; Name: "{userappdata}\obs-studio\plugins\obs-input-logger"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\obs-plugins\64bit\obs-input-logger.dll"
Type: filesandordirs; Name: "{app}\obs-plugins\64bit\obs-input-logger.pdb"
Type: filesandordirs; Name: "{app}\data\obs-plugins\obs-input-logger"

[Code]
function InitializeSetup(): Boolean;
var
  ObsExe: string;
begin
  Result := True;
  // If the default directory doesn't contain obs64.exe, warn — user probably
  // installed OBS somewhere non-standard and needs to point us at it.
  ObsExe := ExpandConstant('{commonpf}\obs-studio\bin\64bit\obs64.exe');
  if not FileExists(ObsExe) then
    MsgBox('OBS Studio was not found at ' + ExpandConstant('{commonpf}\obs-studio') + '.' + #13#10 + #13#10 +
           'On the next screen, please point the installer at your OBS install folder ' +
           '(the folder that contains the "bin" and "data" subfolders).',
           mbInformation, MB_OK);
end;

[Messages]
WelcomeLabel2=This will install [name/ver] into your OBS Studio folder.%n%nWindows will ask for administrator permission — that's required to write into Program Files.%n%nAfter the installer finishes, start OBS and open the Tools menu — you'll see "Input Logger: Enabled". Each recording will produce a .inputlog.jsonl file next to the video.
