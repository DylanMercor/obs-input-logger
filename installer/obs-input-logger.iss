; Inno Setup script — builds a double-click Windows installer for obs-input-logger.
;
; The OBS plugin layout on Windows (per-user) is:
;   %APPDATA%\obs-studio\plugins\obs-input-logger\bin\64bit\obs-input-logger.dll
;   %APPDATA%\obs-studio\plugins\obs-input-logger\data\locale\en-US.ini
;
; This installer drops those files in place for the current user — no admin
; rights required, no merging folders by hand.

#ifexist "defines.isi"
  #include "defines.isi"
#endif

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#ifndef StagingDir
  ; Default assumes ISCC is invoked from repo root (the CI pattern).
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

[Files]
Source: "{#StagingDir}\bin\64bit\obs-input-logger.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#StagingDir}\bin\64bit\obs-input-logger.pdb"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StagingDir}\data\*";                        DestDir: "{app}\data";      Flags: ignoreversion recursesubdirs createallsubdirs

[Messages]
WelcomeLabel2=This will install [name/ver] into your personal OBS Studio plugins folder.%n%nAfter the installer finishes, restart OBS and you'll see "Input Logger: Enabled" under the Tools menu. Each recording will automatically produce a .inputlog.jsonl file next to the video.%n%nNo administrator password needed.
