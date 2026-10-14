; Inno Setup 6. CI passes /DAppVersion from src/win/version.h; the fallback below must match it.
#ifndef AppVersion
  #define AppVersion "1.0.1"
#endif
#ifndef BinDir
  #define BinDir "..\build\Release"
#endif

[Setup]
AppId={{6F1C2E7A-3B7D-4C55-9C2E-CA57EC0DE100}
AppName=Cast Recorder
AppVersion={#AppVersion}
AppVerName=Cast Recorder {#AppVersion}
AppPublisher=NeusByCast
AppPublisherURL=https://github.com/NeusByCast/cast-recorder
DefaultDirName={autopf}\Cast Recorder
DefaultGroupName=Cast Recorder
DisableProgramGroupPage=yes
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\dist
OutputBaseFilename=CastRecorder-Setup-{#AppVersion}
SetupIconFile=..\res\icon.ico
UninstallDisplayIcon={app}\CastRecorder.exe
UninstallDisplayName=Cast Recorder
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "autostart"; Description: "Start Cast Recorder in the tray when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "{#BinDir}\CastRecorder.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Start menu entry: this is what makes "Cast Recorder" show up in Windows search.
Name: "{autoprograms}\Cast Recorder"; Filename: "{app}\CastRecorder.exe"; AppUserModelID: "NeusByCast.CastRecorder"
Name: "{autodesktop}\Cast Recorder"; Filename: "{app}\CastRecorder.exe"; Tasks: desktopicon
Name: "{userstartup}\Cast Recorder"; Filename: "{app}\CastRecorder.exe"; Parameters: "--minimized"; Tasks: autostart

[Run]
Filename: "{app}\CastRecorder.exe"; Description: "Launch Cast Recorder"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /IM CastRecorder.exe /F"; Flags: runhidden; RunOnceId: "KillApp"

[Code]
// Settings live in %APPDATA%\Cast Recorder. Ask before deleting them; the default is to keep them.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Dir := ExpandConstant('{userappdata}\Cast Recorder');
    if DirExists(Dir) then
      if SuppressibleMsgBox('Also delete your Cast Recorder settings?' + #13#10 + Dir + #13#10#13#10 +
                            'Your recordings are not affected.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
        DelTree(Dir, True, True, True);
  end;
end;
