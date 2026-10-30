#pragma once

// Shared by the mainwindow*.cpp / mw_*.cpp files that implement MainWindow.

#include "mainwindow.h"

#include <cmath>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace {

enum : int {
    ID_NEW = 200, ID_NEW_PROJECT, ID_OPEN_FILE, ID_OPEN_FOLDER, ID_SAVE, ID_SAVE_AS, ID_SAVE_ALL, ID_CLOSE_TAB,
    ID_CLOSE_PROJECT, ID_EXIT, ID_UNDO, ID_REDO, ID_FIND, ID_REPLACE, ID_FIND_NEXT, ID_FIND_PREV, ID_GOTO,
    ID_COMMENT, ID_DUPLICATE, ID_ZOOM_IN, ID_ZOOM_OUT, ID_ZOOM_RESET, ID_BUILD, ID_RUN, ID_STOP, ID_CLEAR_CONSOLE,
    ID_RESET_MAIN, ID_SETTINGS, ID_NEXT_TAB, ID_PREV_TAB, ID_REFRESH_TREE,
    ID_DEBUG, ID_STEP_OVER, ID_STEP_INTO, ID_STEP_OUT, ID_PAUSE, ID_TOGGLE_BP, ID_CLEAR_BPS, ID_DEBUG_PANEL,
    ID_COMPLETE, ID_REBUILD, ID_GOTO_DEF, ID_NAV_BACK, ID_NAV_FORWARD, ID_FIND_USAGES, ID_RENAME, ID_SHOW_HINT,
    ID_MENU_FILE = 300, ID_MENU_EDIT, ID_MENU_VIEW, ID_MENU_RUN,
    ID_RECENT = 400,
    ID_TREE_NEW_FILE = 500, ID_TREE_NEW_FOLDER, ID_TREE_OPEN, ID_TREE_MAIN, ID_TREE_RENAME, ID_TREE_DELETE,
    ID_TREE_REVEAL, ID_TREE_EXCLUDE,
    ID_TREE_CTRL = 601, ID_MEM_EXPR = 603,
};

// Styles of the debug panel views.
enum PanelStyle : int { P_TEXT = 0, P_LABEL, P_CHANGED, P_HEADER, P_SYMBOL, P_FLAG_ON, P_DIM, P_ERROR, P_ADDR };

enum : UINT {
    WM_APP_OUTPUT = WM_APP + 1,   // wp = Out, lp = std::string*
    WM_APP_BUILD_DONE,            // lp = BuildDone*
    WM_APP_PROC_EXIT,             // wp = exit code, lp = generation
    WM_APP_CHECK_DONE,            // lp = CheckDone*
    WM_APP_TOOL_STATUS,           // wp = warn, lp = std::wstring*
    WM_APP_PTY_OUTPUT,            // wp = console session, lp = std::string* (raw VT output)
    WM_APP_PTY_RENDER,            // repaint the terminal after a batch of output
    WM_APP_PTY_DONE,              // wp = exit code, lp = console session
};

constexpr UINT_PTR kCheckTimerBase = 1000;
constexpr UINT_PTR kConsoleFocusTimer = 2;

struct BuildDone {
    BuildResult result;
    int after;
    fs::path src;
};

inline std::string Hex16(uint64_t v) {
    char buf[20];
    sprintf_s(buf, "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

inline std::string PadRight(std::string s, size_t n) {
    if (s.size() < n) s.append(n - s.size(), ' ');
    return s;
}


struct CheckDone {
    int editorId;
    int version;
    std::vector<Diagnostic> diagnostics;
};

const char* kTemplate =
    "; Hello, world для Windows x64 (NASM + MSVC link + C runtime)\r\n"
    "default rel\r\n"
    "\r\n"
    "extern printf\r\n"
    "\r\n"
    "global main\r\n"
    "\r\n"
    "section .data\r\n"
    "    msg     db \"Hello, world!\", 10, 0\r\n"
    "\r\n"
    "section .text\r\n"
    "main:\r\n"
    "    sub     rsp, 40             ; shadow space (32 байта) + выравнивание стека на 16\r\n"
    "    lea     rcx, [msg]          ; 1-й аргумент — в rcx\r\n"
    "    call    printf\r\n"
    "    add     rsp, 40\r\n"
    "    xor     eax, eax            ; return 0\r\n"
    "    ret\r\n";

const std::set<std::wstring> kHiddenDirs = {L".asmeditor", L".git", L".idea", L".vs", L".vscode"};

inline void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline int TextWidth(HDC dc, const std::wstring& s) {
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    return sz.cx;
}

inline void DrawTextColor(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT flags) {
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX);
}

inline bool Contains(const RECT& r, int x, int y) {
    POINT p{x, y};
    return PtInRect(&r, p) != 0;
}

inline std::map<std::wstring, std::wstring> ReadKeyValues(const fs::path& file) {
    std::map<std::wstring, std::wstring> kv;
    std::ifstream in(file, std::ios::binary);
    if (!in) return kv;
    std::stringstream ss;
    ss << in.rdbuf();
    for (const std::wstring& line : Split(Widen(ss.str()), L'\n')) {
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) kv[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return kv;
}

}
