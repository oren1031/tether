; The Windows installer, built by the release workflow:
;   iscc /DVersion=0.3.0 /DSource=<folder windeployqt filled> packaging\windows\tether.iss
; Installs for the current user, so it needs no administrator rights and
; Tether can update itself the same way.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef Source
  #define Source "..\..\build\bin"
#endif

[Setup]
AppId={{6F2B1C9E-7D41-4C1A-9B57-3E0A5D7C4E21}
AppName=Tether
AppVersion={#Version}
AppPublisher=Tether
AppPublisherURL=https://github.com/oren1031/tether
AppSupportURL=https://github.com/oren1031/tether/issues
AppUpdatesURL=https://github.com/oren1031/tether/releases
DefaultDirName={localappdata}\Programs\Tether
DefaultGroupName=Tether
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\..\dist
OutputBaseFilename=Tether-{#Version}-setup
SetupIconFile=..\..\pkgbuild\tether.ico
UninstallDisplayIcon={app}\tether.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809 is the first with ConPTY, which the terminal needs.
MinVersion=10.0.17763
CloseApplications=yes
RestartApplications=yes
LicenseFile=..\..\LICENSE

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#Source}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Tether"; Filename: "{app}\tether.exe"
Name: "{autodesktop}\Tether"; Filename: "{app}\tether.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\tether.exe"; Description: "{cm:LaunchProgram,Tether}"; Flags: nowait postinstall skipifsilent
