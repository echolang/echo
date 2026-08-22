; GUI wizard for Echo. Built by the Windows release job:
;
;   iscc /DAppVersion=0.2.3 /DStageDir=... tools/windows/echo.iss
;
; binaries go in {app}\bin, which defaults to %LOCALAPPDATA%\echo\bin, the
; same directory install.ps1 uses. clang, lld-link and sysroot sit next to
; them so `echoc build` needs no separate LLVM. per-user, no admin.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "stage"
#endif

#define AppName "Echo"
#define AppPublisher "echolang"
#define AppURL "https://github.com/echolang/echo"

[Setup]
AppId={{E4C80A31-9B7F-4D2A-A6F1-3C8E5B7A1D90}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}/releases
DefaultDirName={localappdata}\echo
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile={#StageDir}\LICENSE
OutputDir=.
OutputBaseFilename=echo-windows-x86_64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ChangesEnvironment=yes
UninstallDisplayName={#AppName} {#AppVersion}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupLogging=yes
UsePreviousAppDir=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#StageDir}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#StageDir}\sysroot\*"; DestDir: "{app}\sysroot"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Messages]
FinishedLabel=Echo is installed. Open a new terminal and run echoc --version.

[Code]
function NeedsAddPath(BinDir: string): Boolean;
var
  Current: string;
  Needle: string;
begin
  Result := True;
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current) then
    Exit;
  Needle := ';' + Uppercase(BinDir) + ';';
  if Pos(Needle, ';' + Uppercase(Current) + ';') > 0 then
    Result := False;
end;

procedure AddToUserPath(BinDir: string);
var
  Current: string;
begin
  if not NeedsAddPath(BinDir) then
    Exit;
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current) then
    Current := '';
  if Current = '' then
    Current := BinDir
  else if Current[Length(Current)] = ';' then
    Current := Current + BinDir
  else
    Current := Current + ';' + BinDir;
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current);
end;

procedure RemoveFromUserPath(BinDir: string);
var
  Current, Entry, ResultPath: string;
  StartPos, Semi: Integer;
  Want: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current) then
    Exit;
  Want := Uppercase(BinDir);
  ResultPath := '';
  Current := Current + ';';
  StartPos := 1;
  while StartPos <= Length(Current) do
  begin
    Semi := StartPos;
    while (Semi <= Length(Current)) and (Current[Semi] <> ';') do
      Semi := Semi + 1;
    Entry := Copy(Current, StartPos, Semi - StartPos);
    if (Entry <> '') and (Uppercase(Entry) <> Want) then
    begin
      if ResultPath <> '' then
        ResultPath := ResultPath + ';';
      ResultPath := ResultPath + Entry;
    end;
    StartPos := Semi + 1;
  end;
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', ResultPath);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddToUserPath(ExpandConstant('{app}\bin'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromUserPath(ExpandConstant('{app}\bin'));
end;
