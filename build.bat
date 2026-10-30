@echo off
setlocal
cd /d "%~dp0"

rem Usage: build.bat          - AsmEditor.exe
rem        build.bat test     - also build and run the unit tests (tests\tests.cpp)
rem        build.bat package  - also make AsmEditor-portable.zip

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

set LIBS=user32.lib gdi32.lib imm32.lib ole32.lib oleaut32.lib uuid.lib msimg32.lib comctl32.lib comdlg32.lib ^
 shell32.lib shlwapi.lib dwmapi.lib uxtheme.lib advapi32.lib dbghelp.lib winhttp.lib

if not exist out mkdir out
rc /nologo /fo out\app.res src\app.rc || exit /b 1
cl /nologo /std:c++20 /utf-8 /EHsc /O2 /GL /MT /W3 /DUNICODE /D_UNICODE ^
   /Ithird_party\scintilla\include /Foout\ src\*.cpp out\app.res /Fe:AsmEditor.exe ^
   /link /LTCG /SUBSYSTEM:WINDOWS /MANIFEST:EMBED third_party\scintilla\bin\libscintilla.lib %LIBS% || exit /b 1
echo Built AsmEditor.exe

if /i "%1"=="test" (
    if not exist out\tests mkdir out\tests
    rem The unit tests use the UI-independent sources only.
    cl /nologo /std:c++20 /utf-8 /EHsc /O1 /MT /W3 /DUNICODE /D_UNICODE /Ithird_party\scintilla\include /Isrc ^
       /Foout\tests\ tests\tests.cpp src\syntax.cpp src\completion.cpp src\toolchain.cpp src\project.cpp ^
       src\util.cpp src\theme.cpp src\log.cpp src\l10n.cpp src\asmtools.cpp src\terminal.cpp src\hints.cpp ^
       src\debugger.cpp /Fe:out\tests\tests.exe /link %LIBS% || exit /b 1
    out\tests\tests.exe || exit /b 1
)

if /i "%1"=="package" (
    powershell -NoProfile -Command "Compress-Archive -Force -Path AsmEditor.exe,README.md,ROADMAP.md -DestinationPath AsmEditor-portable.zip" || exit /b 1
    echo Packed AsmEditor-portable.zip
)
