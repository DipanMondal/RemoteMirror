#define MyAppName "ASUS_ Optimization"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "LightDesk"
#define MyBuildDir "..\..\build\Release"

[Setup]
AppId={{DA1D3942-6E1B-4CB5-AB15-07B14D68D1DF}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\ASUS Optimization
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=../release
OutputBaseFilename=ASUS_Optimization_Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesEnvironment=yes
CloseApplications=yes

[Files]
Source: "{#MyBuildDir}\ASUS_Optimization.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyBuildDir}\ltdesk.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Open {#MyAppName}"; Filename: "{app}\ltdesk.exe"; Parameters: "up"
Name: "{group}\Stop {#MyAppName}"; Filename: "{app}\ltdesk.exe"; Parameters: "down"

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""ASUS_ Optimization Discovery"" dir=in action=allow program=""{app}\ASUS_Optimization.exe"" protocol=UDP localport=50500 profile=private"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""ASUS_ Optimization ControlVideo"" dir=in action=allow program=""{app}\ASUS_Optimization.exe"" protocol=TCP localport=50510-50511 profile=private"; Flags: runhidden

[UninstallRun]
Filename: "{app}\ltdesk.exe"; Parameters: "down"; Flags: runhidden skipifdoesntexist
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""ASUS_ Optimization Discovery"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""ASUS_ Optimization ControlVideo"""; Flags: runhidden

[Code]
const
  EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  HWND_BROADCAST = $FFFF;
  WM_SETTINGCHANGE = $001A;
  SMTO_ABORTIFHUNG = $0002;

function SendMessageTimeout(hWnd: LongWord; Msg: LongWord; wParam: LongWord; lParam: String; fuFlags: LongWord; uTimeout: LongWord; var lpdwResult: LongWord): LongWord;
  external 'SendMessageTimeoutW@user32.dll stdcall';

procedure BroadcastEnvironmentChange();
var
  ResultCode: LongWord;
begin
  SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 'Environment', SMTO_ABORTIFHUNG, 5000, ResultCode);
end;

function PathContains(PathValue: String; DirValue: String): Boolean;
var
  P: String;
  D: String;
begin
  P := ';' + Uppercase(PathValue) + ';';
  D := ';' + Uppercase(DirValue) + ';';
  StringChangeEx(P, ';;', ';', True);
  Result := Pos(D, P) > 0;
end;

procedure AddAppToMachinePath();
var
  PathValue: String;
  AppDir: String;
  NewValue: String;
begin
  AppDir := ExpandConstant('{app}');

  if not RegQueryStringValue(HKLM, EnvironmentKey, 'Path', PathValue) then begin
    PathValue := '';
  end;

  if PathContains(PathValue, AppDir) then begin
    exit;
  end;

  if PathValue = '' then begin
    NewValue := AppDir;
  end else begin
    NewValue := PathValue + ';' + AppDir;
  end;

  RegWriteExpandStringValue(HKLM, EnvironmentKey, 'Path', NewValue);
  BroadcastEnvironmentChange();
end;

function RemoveDirFromPath(PathValue: String; DirValue: String): String;
var
  S: String;
begin
  S := PathValue;
  StringChangeEx(S, ';' + DirValue, '', True);
  StringChangeEx(S, DirValue + ';', '', True);

  if CompareText(S, DirValue) = 0 then begin
    S := '';
  end;

  Result := S;
end;

procedure RemoveAppFromMachinePath();
var
  PathValue: String;
  AppDir: String;
  NewValue: String;
begin
  AppDir := ExpandConstant('{app}');

  if RegQueryStringValue(HKLM, EnvironmentKey, 'Path', PathValue) then begin
    NewValue := RemoveDirFromPath(PathValue, AppDir);

    if NewValue <> PathValue then begin
      RegWriteExpandStringValue(HKLM, EnvironmentKey, 'Path', NewValue);
      BroadcastEnvironmentChange();
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    AddAppToMachinePath();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    RemoveAppFromMachinePath();
  end;
end;
