; SPDX-License-Identifier: GPL-2.0-or-later
; Inno Setup script for the Yunseul MR filter (Windows, 64-bit).
; Built by .github/scripts/Package-Windows.ps1, which passes:
;   /DMyAppVersion=  /DSourceDir=  /DProjectRoot=  /DOutputDir=  /DOutputName=

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef OutputName
  #define OutputName "yunseul-mr-windows-x64-Installer"
#endif

[Setup]
AppId={{2530479F-C547-4055-A109-C3CAAF1EA579}
AppName=Yunseul MR
AppVersion={#MyAppVersion}
AppVerName=Yunseul MR {#MyAppVersion}
AppPublisher=Yunseul
AppPublisherURL=https://github.com/yunseulmp3/yunseul-mr
AppSupportURL=https://github.com/yunseulmp3/yunseul-mr
DefaultDirName={commonappdata}\obs-studio\plugins\yunseul-mr
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=no
UninstallDisplayName=Yunseul MR
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
#if FileExists(AddBackslash(CompilerPath) + "Languages\Korean.isl")
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"
#endif

[Files]
Source: "{#SourceDir}\yunseul-mr\bin\64bit\yunseul-mr.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#SourceDir}\yunseul-mr\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#ProjectRoot}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
