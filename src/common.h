#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Scintilla.h"

namespace fs = std::filesystem;

// ---- strings & paths ------------------------------------------------------------------------------------------

std::wstring Widen(const std::string& s, UINT codePage = CP_UTF8);
std::string Narrow(const std::wstring& s, UINT codePage = CP_UTF8);
std::wstring Trim(const std::wstring& s);
std::wstring Lower(std::wstring s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);
std::wstring Join(const std::vector<std::wstring>& parts, const std::wstring& sep);
fs::path NormalizePath(const fs::path& p);
bool SamePath(const fs::path& a, const fs::path& b);
bool IsUnder(const fs::path& p, const fs::path& dir);
bool FileExists(const std::wstring& path);
bool DirExists(const fs::path& path);

// ---- theme ----------------------------------------------------------------------------------------------------

constexpr COLORREF Hex(uint32_t rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Colors of the current theme (see Theme::Apply). Everything paints from these, so switching the theme only needs
// the styles re-applied and a repaint.
namespace Theme {
extern COLORREF EditorBg, PanelBg, InputBg, Border, Hover, Pressed, Text, Bright, Muted, Dim, Accent, Selection,
    InactiveSelection, CurrentLine, Error, Warning, Success, Link, CallTipBg, LineNumber, Changed;
// syntax
extern COLORREF Comment, String, Number, Register, Instruction, Directive, Preprocessor, Label, Keyword, Macro,
    Function;
// markers
extern COLORREF ErrorLine, WarningLine, Breakpoint, ConditionalBreakpoint, ExecArrow, ExecLine, ListSelected,
    Occurrence;
extern COLORREF Ansi[16];
extern bool Dark;

struct Info {
    const wchar_t* id;
    const wchar_t* title;
};
const std::vector<Info>& All();
void Apply(const std::wstring& id);  // unknown ids fall back to the default dark theme
COLORREF Blend(COLORREF a, COLORREF b, int percentOfB);
}

void InitDarkMode();
void ApplyDarkWindow(HWND hwnd);       // dark title bar
void ApplyDarkControl(HWND hwnd);      // dark scrollbars etc.
HFONT CreateUiFont(int dpi, int points = 9, bool bold = false);
std::wstring MonospaceFontName();
int Scale(int value, int dpi);

// Flat owner-painted button (also used as a checkbox with FB_CHECK). Sends WM_COMMAND(id, BN_CLICKED).
constexpr DWORD FB_CHECK = 0x1;
constexpr DWORD FB_PRIMARY = 0x2;
void RegisterFlatButton(HINSTANCE inst);
HWND CreateFlatButton(HWND parent, int id, const wchar_t* text, DWORD flags = 0);

// ---- diagnostics ----------------------------------------------------------------------------------------------

enum class Severity { Error, Warning };

struct Diagnostic {
    fs::path file;   // empty for live checks of unsaved text
    int line = 0;    // 1-based
    Severity severity = Severity::Error;
    std::wstring message;
    std::string code;  // what kind of problem, for quick fixes ("undefined:printf", "callee-saved:rbx", "div-rdx"...)
};

// ---- settings -------------------------------------------------------------------------------------------------

struct Settings {
    std::wstring nasmPath;
    std::wstring nasmArgs;
    std::wstring linkerPreset;
    std::wstring linkerPath;
    std::wstring linkerArgs;
    std::vector<std::wstring> libPaths;
    std::wstring releaseNasmArgs;    // empty: the debug arguments without -g / -F
    std::wstring releaseLinkerArgs;  // empty: the debug arguments without /debug
    std::wstring programEncoding = L"UTF-8";
    bool externalConsole = false;
    bool liveCheck = true;
    bool stackCheck = true;       // warn about a misaligned stack / missing shadow space before call
    bool autosave = false;        // save every file when the window loses focus
    bool formatOnSave = false;    // align the columns (Ctrl+Alt+L) of .asm files when saving
    bool log = false;             // write %APPDATA%\AsmEditor\asmeditor.log
    std::wstring theme = L"dark";
    std::wstring language = L"ru";
    int formatIndent = 4;         // code formatter: column of mnemonics,
    int formatOperands = 12;      // of operands
    int formatComments = 40;      // and of trailing comments (0-based)
    std::map<std::wstring, std::wstring> keys;  // command name -> shortcut ("" = none), only changed ones
    int fontSize = 11;
    std::vector<std::wstring> recent;
    std::wstring lastProject;
    std::vector<std::wstring> sessionFiles;
    RECT window{};
    bool maximized = false;
    int treeWidth = 240;
    int consoleHeight = 230;
    int debugWidth = 430;

    void Load();
    void Save() const;
    static fs::path Dir();
};

// ---- syntax ---------------------------------------------------------------------------------------------------

enum TokenKind : int {
    T_DEFAULT = 0, T_COMMENT, T_STRING, T_NUMBER, T_REGISTER, T_INSTRUCTION,
    T_DIRECTIVE, T_PREPROCESSOR, T_LABEL, T_KEYWORD, T_COUNT
};

struct Token {
    int start;
    int end;
    TokenKind type;
};

void Tokenize(const char* line, int length, std::vector<Token>& out);

enum class WordClass { Register, Instruction, Directive, DataDirective, Keyword, Prefix };
std::vector<std::string> SyntaxWords(WordClass cls);  // lower-case words known to the highlighter

// ---- toolchain ------------------------------------------------------------------------------------------------

enum class Out { Normal, Error, Warning, Info, Success, Command, Input };
using Sink = std::function<void(const std::string& utf8, Out style)>;

struct LinkerPreset {
    const wchar_t* id;
    const wchar_t* title;
    const wchar_t* args;  // nullptr for "custom"
};
extern const LinkerPreset kLinkerPresets[];
extern const int kLinkerPresetCount;
const LinkerPreset* FindPreset(const std::wstring& id);

struct MsvcInfo {
    fs::path link;
    std::vector<fs::path> libPaths;
};

// Per-project settings of how the program is built and started (stored in .asmeditor/project.ini).
struct RunConfig {
    std::wstring args;       // command line arguments
    std::wstring workDir;    // relative to the project; empty = project folder
    std::wstring env;        // NAME=value lines
    std::wstring stdinFile;  // its contents are typed into the program at start
    std::wstring output = L"exe";       // exe | dll | lib
    std::wstring profile = L"Debug";    // Debug | Release
    std::wstring platform = L"windows"; // windows | wsl (Linux, -f elf64) | wsl32 (Linux, -f elf32)
    std::wstring preBuild;   // commands run in the project folder before / after the build
    std::wstring postBuild;
    bool Release() const { return profile == L"Release"; }
    bool Wsl() const { return platform == L"wsl" || platform == L"wsl32"; }
    bool Linux32() const { return platform == L"wsl32"; }
};

struct BuildPlan {
    std::vector<fs::path> sources;    // assembled separately and linked together; the first is the target
    fs::path root;                    // object files mirror the folders below it
    fs::path outDir;                  // build folder
    fs::path exe;                     // output file (.exe, .dll, .lib or a Linux program)
    std::vector<fs::path> includes;   // extra %include search folders (the project root)
    bool rebuild = false;             // ignore up-to-date object files
    RunConfig config;
};

struct BuildResult {
    bool ok = false;
    fs::path exe;
    std::vector<Diagnostic> diagnostics;
};

namespace Toolchain {
inline const wchar_t* kDefaultNasmArgs = L"-f win64 -g -F cv8";

std::optional<fs::path> FindNasm();
std::optional<fs::path> FindOnPath(const wchar_t* exe);
std::optional<MsvcInfo> FindMsvc(bool x86 = false);  // x86: 32-bit link.exe and libraries
void ApplyDefaults(Settings& s);
void DetectLinker(Settings& s);
bool NasmAvailable(const Settings& s);
bool LinkerAvailable(const Settings& s);
std::wstring NasmVersion(const Settings& s);

std::vector<std::wstring> SplitArgs(const std::wstring& s);
std::wstring QuoteCommandLine(const std::vector<std::wstring>& args);

// Starts a process with the given std handles (only these are inherited). Returns false on failure.
bool Spawn(const std::wstring& commandLine, const fs::path& cwd, HANDLE in, HANDLE out, HANDLE err,
           DWORD flags, PROCESS_INFORMATION& pi);
// Starts a process attached to a pseudoconsole (ConPTY) instead of pipes.
bool SpawnInConsole(const std::wstring& commandLine, const fs::path& cwd, HPCON console, DWORD flags,
                    PROCESS_INFORMATION& pi, const std::wstring& environment = {});
// Sets the input/output code page of a pseudoconsole before the program is started in it.
void SetConsoleCodePage(HPCON console, UINT codePage);
// Runs a tool to completion; each output line (converted to UTF-8) goes to onLine and into capture.
int Exec(const std::vector<std::wstring>& cmd, const fs::path& cwd,
         const std::function<void(const std::string&)>& onLine, std::string* capture);

std::vector<Diagnostic> ParseNasm(const std::string& output);
BuildResult Build(const Settings& s, const BuildPlan& plan, const Sink& sink);
std::vector<Diagnostic> Check(const Settings& s, const RunConfig& config, const std::string& text, int id,
                              const std::vector<fs::path>& includes);
std::wstring DescribeExitCode(DWORD code);
// NASM / linker arguments for the profile (Release drops the debug information switches).
std::wstring NasmArgs(const Settings& s, const RunConfig& c);
std::wstring LinkerArgs(const Settings& s, const RunConfig& c);
std::wstring WslPath(const fs::path& windowsPath);  // D:\a\b -> /mnt/d/a/b
std::optional<fs::path> FindNdisasm(const Settings& s);
// Environment block for CreateProcess: the current environment plus NAME=value lines.
std::wstring EnvironmentBlock(const std::wstring& extraLines);
// "1.2 с · пик памяти 3.4 МБ" for a finished process (handle still open).
std::string ProcessStats(HANDLE process, ULONGLONG startTick);
}

// ---- log ------------------------------------------------------------------------------------------------------

// Diagnostic log of the editor itself (%APPDATA%\AsmEditor\asmeditor.log), on when Settings::log is set or
// ASMEDITOR_LOG=1.
namespace Log {
void Enable(bool on);
bool Enabled();
void Write(const std::string& utf8);
fs::path File();
}

// "Open with": AsmEditor as the program for .asm, .inc, .nasm and .mac files of the current user (HKCU, no admin
// rights). `classes` is the registry key under HKEY_CURRENT_USER (another one in the tests).
namespace FileAssociation {
bool Register(const fs::path& exe, std::wstring& error, const std::wstring& classes = L"Software\\Classes");
bool IsRegistered(const fs::path& exe, const std::wstring& classes = L"Software\\Classes");
void Unregister(const std::wstring& classes = L"Software\\Classes");
}

// ---- localization ---------------------------------------------------------------------------------------------

// The UI is written in Russian; Tr returns the English text when the English language is selected.
void SetLanguage(const std::wstring& id);  // "ru" | "en"
bool English();
const wchar_t* Tr(const wchar_t* russian);
std::string Tr(const std::string& russianUtf8);

// ---- dialogs --------------------------------------------------------------------------------------------------

std::optional<std::wstring> InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt,
                                     const std::wstring& initial);
bool ShowSettingsDialog(HWND owner, Settings& settings);
void ShowMessage(HWND owner, const std::wstring& text, bool error = true);

// Popup list with a filter box (command palette, quick open, templates). Items are "text\tdetail".
// Returns the index of the chosen item.
std::optional<int> PickFromList(HWND owner, const std::wstring& title, const std::vector<std::wstring>& items,
                                const std::wstring& placeholder = L"");

struct FindInFilesRequest {
    std::wstring what, replaceWith;
    bool matchCase = false, wholeWord = false, regex = false, replace = false;
};
std::optional<FindInFilesRequest> ShowFindInFilesDialog(HWND owner, const FindInFilesRequest& initial);
bool ShowRunConfigDialog(HWND owner, RunConfig& config);
// Edits Settings::keys; `commands` are (name, default shortcut).
bool ShowKeysDialog(HWND owner, Settings& settings, const std::vector<std::pair<std::wstring, std::wstring>>& commands);
