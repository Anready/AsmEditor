@echo off
setlocal
cd /d "%~dp0"

rem Find Visual Studio with the C++ tools and set up the x64 environment.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio C++ tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1

rem Scintilla (static library) is built once.
if not exist third_party\scintilla\bin\libscintilla.lib (
    if not exist third_party\scintilla\win32\obj mkdir third_party\scintilla\win32\obj
    pushd third_party\scintilla\win32
    nmake -nologo -f scintilla.mak ..\bin\libscintilla.lib || (popd & exit /b 1)
    popd
)

if not exist out mkdir out
cl /nologo /std:c++20 /utf-8 /EHsc /O2 /GL /MT /W3 /DUNICODE /D_UNICODE ^
   /Ithird_party\scintilla\include /Foout\ src\*.cpp /Fe:AsmEditor.exe ^
   /link /LTCG /SUBSYSTEM:WINDOWS /MANIFEST:EMBED third_party\scintilla\bin\libscintilla.lib ^
   user32.lib gdi32.lib imm32.lib ole32.lib oleaut32.lib uuid.lib msimg32.lib comctl32.lib comdlg32.lib ^
   shell32.lib shlwapi.lib dwmapi.lib uxtheme.lib advapi32.lib dbghelp.lib || exit /b 1
echo Built AsmEditor.exe
