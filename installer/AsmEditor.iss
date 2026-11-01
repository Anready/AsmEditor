; Inno Setup 6 script: build.bat installer (or iscc installer\AsmEditor.iss after build.bat).
; Installs for the current user without admin rights by default (the user may choose all users),
; adds a Start menu shortcut and, optionally, opens .asm/.inc/.nasm/.mac files with AsmEditor.

#define AppVersion GetEnv("ASMEDITOR_VERSION")
#if AppVersion == ""
  #define AppVersion "1.5.0"
#endif

[Setup]
AppId={{6F4E2B1A-8C3D-4F5E-9A7B-2D1C0E9F8A61}
AppName=AsmEditor
AppVersion={#AppVersion}
AppPublisher=AsmEditor
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\AsmEditor
DefaultGroupName=AsmEditor
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
OutputDir=..\out\installer
OutputBaseFilename=AsmEditor-setup-{#AppVersion}
SetupIconFile=..\assets\app.ico
UninstallDisplayIcon={app}\AsmEditor.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
ru.AssocTask=Открывать файлы .asm, .inc, .nasm и .mac в AsmEditor
en.AssocTask=Open .asm, .inc, .nasm and .mac files with AsmEditor
ru.AssocName=Исходный код NASM
en.AssocName=NASM source

[Tasks]
Name: "assoc"; Description: "{cm:AssocTask}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\AsmEditor.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.ru.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\AsmEditor"; Filename: "{app}\AsmEditor.exe"
Name: "{autodesktop}\AsmEditor"; Filename: "{app}\AsmEditor.exe"; Tasks: desktopicon

; The same ProgID as Help → "Open .asm files with AsmEditor" in the editor (FileAssociation in util.cpp).
[Registry]
Root: HKA; Subkey: "Software\Classes\AsmEditor.asm"; ValueType: string; ValueData: "{cm:AssocName}"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\AsmEditor.asm\DefaultIcon"; ValueType: string; ValueData: "{app}\AsmEditor.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\AsmEditor.asm\shell\open\command"; ValueType: string; ValueData: """{app}\AsmEditor.exe"" ""%1"""; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.asm\OpenWithProgids"; ValueType: string; ValueName: "AsmEditor.asm"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.inc\OpenWithProgids"; ValueType: string; ValueName: "AsmEditor.asm"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.nasm\OpenWithProgids"; ValueType: string; ValueName: "AsmEditor.asm"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.mac\OpenWithProgids"; ValueType: string; ValueName: "AsmEditor.asm"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.asm"; ValueType: string; ValueData: "AsmEditor.asm"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.nasm"; ValueType: string; ValueData: "AsmEditor.asm"; Flags: uninsdeletevalue; Tasks: assoc

[Run]
Filename: "{app}\AsmEditor.exe"; Description: "{cm:LaunchProgram,AsmEditor}"; Flags: nowait postinstall skipifsilent
