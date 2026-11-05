; MiniMe Slicer installer (Inno Setup 6). Built by build_installer.ps1, which passes:
;   AppVersion   e.g. 2.8.4.57
;   SourceDir    the installed program (cmake --install ... -> install_dir)
;   CrtDir       Microsoft's Visual C++ runtime DLLs (VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT), put next to the program
;   ImagesDir    the wizard pictures made from tools/branding/art
;   OutputDir    where the setup .exe goes

#define AppName "MiniMe Slicer"
#define AppExe  "minime-slicer.exe"

[Setup]
AppId={{6E2B7C1A-4F0D-4B8E-9C35-2D7A61F0B4C9}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=MiniMe Slicer
AppComments=A slicer for FDM 3D printing, based on Bambu Studio (AGPL-3.0)
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} setup
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; installs for the current user without asking for admin rights; the user can choose "all users" instead
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#SourceDir}\LICENSE.txt
OutputDir={#OutputDir}
OutputBaseFilename=MiniMeSlicer-Setup-{#AppVersion}
SetupIconFile={#SourceDir}\resources\images\BambuStudio.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
WizardImageFile={#ImagesDir}\wizard_164.bmp,{#ImagesDir}\wizard_328.bmp
WizardSmallImageFile={#ImagesDir}\small_55.bmp,{#ImagesDir}\small_110.bmp
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=auto

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "\include,\log_iotc.txt,*.map,*.pdb,*.lib,*.exp"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#CrtDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; files MiniMe writes next to itself while running (settings and projects live in %APPDATA%\MiniMeSlicer and stay)
Type: files; Name: "{app}\log_iotc.txt"
