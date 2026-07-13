; LensLink OBS-plugin installer (Inno Setup 6).
;
; Compiled by the release workflow from the repository root:
;   iscc /DAppVersion=1.2.0 installer\windows\lenslink.iss
; with the plugin already packaged into stage\ (same layout the zip uses).
;
; Installs into the OBS Studio folder (auto-detected from the registry,
; user-overridable) and registers a normal Windows uninstaller.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{7C34D6B2-9A41-4F0E-8B7D-2E5A31C90F6E}
AppName=LensLink for OBS Studio
AppVersion={#AppVersion}
AppPublisher=Exalted Pixels
AppPublisherURL=https://github.com/MyNamesEMurray/LensLink
AppSupportURL=https://github.com/MyNamesEMurray/LensLink/issues
; Everything is laid out relative to the OBS install folder.
DefaultDirName={code:GetObsDir}
DirExistsWarning=no
AppendDefaultDirName=no
DisableProgramGroupPage=yes
; OBS lives in Program Files, so writing there needs elevation.
PrivilegesRequired=admin
; "x64" (not the newer "x64compatible") so any Inno Setup 6.x compiles it.
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
; Keep the uninstaller out of OBS's root folder.
UninstallFilesDir={app}\data\obs-plugins\lenslink
OutputBaseFilename=LensLink-installer-windows-x64
SourceDir=..\..
OutputDir=.
WizardStyle=modern
SolidCompression=yes

[Files]
Source: "stage\obs-plugins\64bit\lenslink.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "stage\data\obs-plugins\lenslink\*"; DestDir: "{app}\data\obs-plugins\lenslink"; Flags: ignoreversion recursesubdirs

[Messages]
SelectDirDesc=Where is OBS Studio installed?
SelectDirLabel3=Setup will install the LensLink plugin into your OBS Studio folder.

[Code]
function GetObsDir(Param: string): string;
begin
  { OBS Studio's installer records its location here. }
  if not RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Result) then
    Result := ExpandConstant('{autopf}\obs-studio');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  { A wrong folder installs "successfully" but OBS never sees the plugin;
    warn (but allow — portable installs may look different). }
  if (CurPageID = wpSelectDir) and
     not FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe')) then
    Result := MsgBox('This folder does not look like an OBS Studio ' +
                     'installation (bin\64bit\obs64.exe not found).' + #13#10 +
                     'Install here anyway?', mbConfirmation, MB_YESNO) = IDYES;
end;
