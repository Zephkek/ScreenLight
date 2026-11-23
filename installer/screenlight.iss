#define AppName "Screen Light"
#define AppVersion "0.1.1"
#define AppExe "ScreenLight.exe"
#define ServiceName "ScreenLight"

[Setup]
AppId={{FB2A10F6-2420-4412-BD27-C108AAB0DB35}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Zeph
VersionInfoVersion={#AppVersion}
VersionInfoCompany=Zeph
VersionInfoDescription={#AppName} setup
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=6.1sp1
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
UsePreviousTasks=yes
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
SetupIconFile=..\src\screenlight.ico
WizardStyle=modern
WizardImageFile=wizard-large-164x314.bmp,wizard-large-205x392.bmp,wizard-large-246x471.bmp,wizard-large-328x628.bmp
WizardSmallImageFile=wizard-small-55x55.bmp,wizard-small-69x69.bmp,wizard-small-83x83.bmp,wizard-small-110x110.bmp
OutputDir=..\out\release
OutputBaseFilename=ScreenLight-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "service"; Description: "Turn on full detection (recommended): see screen recorders, screenshots and keyboard listeners"; Check: IsAdminInstallMode
Name: "autostart"; Description: "Start {#AppName} with Windows"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "..\out\release\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"" --tray"; Tasks: autostart; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\{#AppName}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#AppExe}"; Parameters: "--install"; StatusMsg: "Installing the service..."; Tasks: service; Flags: waituntilterminated
Filename: "{app}\{#AppExe}"; Parameters: "--unelevated"; Description: "Open {#AppName}"; Flags: postinstall nowait skipifsilent
Filename: "{app}\{#AppExe}"; Parameters: "--unelevated --tray"; Flags: nowait; Check: WizardSilent

[UninstallDelete]
Type: files; Name: "{app}\running.txt"

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--uninstall"; RunOnceId: "RemoveService"; Flags: waituntilterminated; Check: IsAdminInstallMode

[Code]
const
  WM_CLOSE = $0010;

function ServiceExists: Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\sc.exe'), 'query {#ServiceName}', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

procedure StopService;
var
  Code, Tries: Integer;
begin
  if not ServiceExists then
    Exit;
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop {#ServiceName}', '', SW_HIDE, ewWaitUntilTerminated, Code);
  for Tries := 1 to 40 do
  begin
    if Exec(ExpandConstant('{cmd}'), '/c sc query {#ServiceName} | find "STOPPED"', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then
      Break;
    Sleep(250);
  end;
  Sleep(500);
end;

procedure CloseTrays;
var
  Wnd: HWND;
  Tries: Integer;
begin
  for Tries := 1 to 20 do
  begin
    Wnd := FindWindowByClassName('ScreenLight');
    if Wnd = 0 then
      Break;
    PostMessage(Wnd, WM_CLOSE, 0, 0);
    Sleep(250);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if IsAdminInstallMode then
    StopService;
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    CloseTrays;
end;
