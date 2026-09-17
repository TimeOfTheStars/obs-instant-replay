; Inno Setup script for the Windows installer. Built by .github/scripts/Package-Windows.ps1 on CI,
; which passes the product name, version and directories as /D defines.
;
; Installs into the per-machine plugin folder OBS scans on start-up
; (C:\ProgramData\obs-studio\plugins\<plugin>\), so it needs administrator rights.

#ifndef ProductName
  #error ProductName must be passed with /DProductName=...
#endif
#ifndef DisplayName
  #define DisplayName ProductName
#endif
#ifndef ProductVersion
  #error ProductVersion must be passed with /DProductVersion=...
#endif
#ifndef SourceDir
  #error SourceDir must be passed with /DSourceDir=...
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Stable id: keeps upgrades replacing the previous version instead of installing side by side.
AppId={{7B0C6D2E-3F5A-4C8B-9E1D-2A6F4B8C0D11}
AppName={#DisplayName}
AppVersion={#ProductVersion}
AppVerName={#DisplayName} {#ProductVersion}
AppPublisher=Trinity AEM
AppPublisherURL=https://github.com/TimeOfTheStars/obs-instant-replay
AppSupportURL=https://github.com/TimeOfTheStars/obs-instant-replay/issues
DefaultDirName={commonappdata}\obs-studio\plugins\{#ProductName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#ProductName}-{#ProductVersion}-windows-x64-installer
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#DisplayName}
UninstallDisplayIcon={sys}\shell32.dll,-16743
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
english.RequiresObs=This plugin requires OBS Studio 32.2 or newer.
russian.RequiresObs=Плагину нужен OBS Studio 32.2 или новее.
english.CloseObs=Close OBS Studio before installing so the new version is loaded on the next start.
russian.CloseObs=Закройте OBS Studio перед установкой — новая версия загрузится при следующем запуске.

[Files]
; Debug symbols stay in the zip; the installer ships only what OBS loads.
Source: "{#SourceDir}\{#ProductName}\bin\64bit\*.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#SourceDir}\{#ProductName}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Messages]
english.WelcomeLabel2=This will install [name/ver] on your computer.%n%n{cm:RequiresObs}%n{cm:CloseObs}
russian.WelcomeLabel2=Программа установит [name/ver] на этот компьютер.%n%n{cm:RequiresObs}%n{cm:CloseObs}

[Code]
function IsObsRunning(): Boolean;
var
  ResultCode: Integer;
begin
  { tasklist exits 0 either way; the filter output is what matters, so check via findstr. }
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq obs64.exe" | findstr /I obs64.exe >nul',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if IsObsRunning() then
    Result := CustomMessage('CloseObs');
end;
