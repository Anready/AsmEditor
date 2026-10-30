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

namespace Theme {
inline constexpr COLORREF EditorBg = Hex(0x1E1F22);
inline constexpr COLORREF PanelBg = Hex(0x2B2D30);
inline constexpr COLORREF InputBg = Hex(0x1E1F22);
inline constexpr COLORREF Border = Hex(0x393B40);
inline constexpr COLORREF Hover = Hex(0x3C3F45);
inline constexpr COLORREF Pressed = Hex(0x4A4D54);
inline constexpr COLORREF Text = Hex(0xBCBEC4);
inline constexpr COLORREF Bright = Hex(0xDFE1E5);
inline constexpr COLORREF Muted = Hex(0x868A91);
inline constexpr COLORREF Dim = Hex(0x5F6369);
inline constexpr COLORREF Accent = Hex(0x3574F0);
inline constexpr COLORREF Selection = Hex(0x214283);
inline constexpr COLORREF CurrentLine = Hex(0x26282E);
inline constexpr COLORREF Error = Hex(0xF75464);
inline constexpr COLORREF Warning = Hex(0xE0B44A);
inline constexpr COLORREF Success = Hex(0x5FB865);
inline constexpr COLORREF Link = Hex(0x56A8F5);
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
};

// ---- settings -------------------------------------------------------------------------------------------------

struct Settings {
    std::wstring nasmPath;
    std::wstring nasmArgs;
    std::wstring linkerPreset;
    std::wstring linkerPath;
    std::wstring linkerArgs;
    std::vector<std::wstring> libPaths;
    std::wstring programEncoding = L"UTF-8";
    bool externalConsole = false;
    bool liveCheck = true;
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

struct BuildPlan {
    std::vector<fs::path> sources;    // assembled separately and linked together; the first is the target
    fs::path root;                    // object files mirror the folders below it
    fs::path outDir;                  // build folder
    fs::path exe;
    std::vector<fs::path> includes;   // extra %include search folders (the project root)
    bool rebuild = false;             // ignore up-to-date object files
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
std::optional<MsvcInfo> FindMsvc();
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
                    PROCESS_INFORMATION& pi);
// Sets the input/output code page of a pseudoconsole before the program is started in it.
void SetConsoleCodePage(HPCON console, UINT codePage);
// Runs a tool to completion; each output line (converted to UTF-8) goes to onLine and into capture.
int Exec(const std::vector<std::wstring>& cmd, const fs::path& cwd,
         const std::function<void(const std::string&)>& onLine, std::string* capture);

std::vector<Diagnostic> ParseNasm(const std::string& output);
BuildResult Build(const Settings& s, const BuildPlan& plan, const Sink& sink);
std::vector<Diagnostic> Check(const Settings& s, const std::string& text, int id,
                              const std::vector<fs::path>& includes);
std::wstring DescribeExitCode(DWORD code);
}

// ---- dialogs --------------------------------------------------------------------------------------------------

std::optional<std::wstring> InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt,
                                     const std::wstring& initial);
bool ShowSettingsDialog(HWND owner, Settings& settings);
void ShowMessage(HWND owner, const std::wstring& text, bool error = true);
