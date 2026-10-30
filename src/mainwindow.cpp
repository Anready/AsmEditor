#include "mainwindow.h"
#include "hints.h"
#include "project.h"

#include <consoleapi.h>

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

std::string Hex16(uint64_t v) {
    char buf[20];
    sprintf_s(buf, "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string PadRight(std::string s, size_t n) {
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

void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

int TextWidth(HDC dc, const std::wstring& s) {
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    return sz.cx;
}

void DrawTextColor(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT flags) {
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX);
}

bool Contains(const RECT& r, int x, int y) {
    POINT p{x, y};
    return PtInRect(&r, p) != 0;
}

std::map<std::wstring, std::wstring> ReadKeyValues(const fs::path& file) {
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

// ---- creation ---------------------------------------------------------------------------------------------------

bool MainWindow::Create(HINSTANCE inst, Settings* settings) {
    inst_ = inst;
    settings_ = settings;
    findMsg_ = RegisterWindowMessageW(FINDMSGSTRINGW);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"AsmEditorMain";
    RegisterClassExW(&wc);

    const RECT& w = settings->window;
    bool hasPos = w.right > 200 && w.bottom > 200;
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, L"AsmEditorMain", L"AsmEditor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            hasPos ? w.left : CW_USEDEFAULT, hasPos ? w.top : CW_USEDEFAULT,
                            hasPos ? w.right : 1280, hasPos ? w.bottom : 820, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    if (!hasPos) {
        // Default size was given in 96-dpi pixels; scale it to the monitor.
        RECT r;
        GetWindowRect(hwnd_, &r);
        SetWindowPos(hwnd_, nullptr, 0, 0, S(1280), S(820), SWP_NOMOVE | SWP_NOZORDER);
    }
    ApplyDarkWindow(hwnd_);

    ACCEL accels[] = {
        {FCONTROL | FVIRTKEY, 'N', ID_NEW},
        {FCONTROL | FVIRTKEY, 'O', ID_OPEN_FILE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'O', ID_OPEN_FOLDER},
        {FCONTROL | FVIRTKEY, 'S', ID_SAVE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'S', ID_SAVE_AS},
        {FCONTROL | FVIRTKEY, 'W', ID_CLOSE_TAB},
        {FCONTROL | FVIRTKEY, 'F', ID_FIND},
        {FCONTROL | FVIRTKEY, 'H', ID_REPLACE},
        {FVIRTKEY, VK_F3, ID_FIND_NEXT},
        {FSHIFT | FVIRTKEY, VK_F3, ID_FIND_PREV},
        {FCONTROL | FVIRTKEY, 'G', ID_GOTO},
        {FCONTROL | FVIRTKEY, VK_SPACE, ID_COMPLETE},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_SPACE, ID_SHOW_HINT},
        {FVIRTKEY, VK_F12, ID_GOTO_DEF},
        {FSHIFT | FVIRTKEY, VK_F12, ID_FIND_USAGES},
        {FVIRTKEY, VK_F2, ID_RENAME},
        {FALT | FVIRTKEY, VK_LEFT, ID_NAV_BACK},
        {FALT | FVIRTKEY, VK_RIGHT, ID_NAV_FORWARD},
        {FCONTROL | FVIRTKEY, VK_OEM_2, ID_COMMENT},
        {FCONTROL | FVIRTKEY, VK_DIVIDE, ID_COMMENT},
        {FCONTROL | FVIRTKEY, VK_OEM_PLUS, ID_ZOOM_IN},
        {FCONTROL | FVIRTKEY, VK_ADD, ID_ZOOM_IN},
        {FCONTROL | FVIRTKEY, VK_OEM_MINUS, ID_ZOOM_OUT},
        {FCONTROL | FVIRTKEY, VK_SUBTRACT, ID_ZOOM_OUT},
        {FCONTROL | FVIRTKEY, '0', ID_ZOOM_RESET},
        {FCONTROL | FVIRTKEY, VK_F9, ID_BUILD},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_F9, ID_REBUILD},
        {FSHIFT | FVIRTKEY, VK_F10, ID_RUN},
        {FCONTROL | FVIRTKEY, VK_F5, ID_RUN},
        {FVIRTKEY, VK_F5, ID_DEBUG},
        {FSHIFT | FVIRTKEY, VK_F5, ID_STOP},
        {FVIRTKEY, VK_F9, ID_TOGGLE_BP},
        {FVIRTKEY, VK_F10, ID_STEP_OVER},
        {FVIRTKEY, VK_F11, ID_STEP_INTO},
        {FSHIFT | FVIRTKEY, VK_F11, ID_STEP_OUT},
        {FCONTROL | FVIRTKEY, VK_F2, ID_STOP},
        {FCONTROL | FALT | FVIRTKEY, 'S', ID_SETTINGS},
        {FCONTROL | FVIRTKEY, VK_TAB, ID_NEXT_TAB},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_TAB, ID_PREV_TAB},
    };
    accel_ = CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));

    checkThread_ = std::thread([this] { CheckWorker(); });
    RefreshToolStatus();

    ShowWindow(hwnd_, settings->maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::CreateFonts() {
    if (uiFont_) DeleteObject(uiFont_);
    if (uiBold_) DeleteObject(uiBold_);
    if (titleFont_) DeleteObject(titleFont_);
    uiFont_ = CreateUiFont(dpi_, 9);
    uiBold_ = CreateUiFont(dpi_, 9, true);
    titleFont_ = CreateUiFont(dpi_, 22, true);
    if (tree_) SendMessageW(tree_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
}

void MainWindow::CreateChildren() {
    dpi_ = GetDpiForWindow(hwnd_);
    inputBrush_ = CreateSolidBrush(Theme::InputBg);

    tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT |
                                TVS_TRACKSELECT | TVS_NOHSCROLL | TVS_INFOTIP,
                            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(ID_TREE_CTRL), inst_, nullptr);
    ApplyDarkControl(tree_);
    TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    TreeView_SetBkColor(tree_, Theme::PanelBg);
    TreeView_SetTextColor(tree_, Theme::Text);
    SHFILEINFOW sfi{};
    auto images = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    TreeView_SetImageList(tree_, images, TVSIL_NORMAL);

    console_ = std::make_unique<Editor>(hwnd_, settings_->fontSize);
    Editor& c = *console_;
    c.Call(SCI_SETMARGINWIDTHN, 0, 0);
    c.Call(SCI_SETMARGINWIDTHN, 1, 0);
    c.Call(SCI_SETMARGINWIDTHN, 2, 6);
    c.Call(SCI_SETCARETLINEVISIBLEALWAYS, 0);
    c.Call(SCI_SETCARETLINEVISIBLE, 0);
    c.Call(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    c.Call(SCI_SETWRAPMODE, SC_WRAP_WORD);
    c.Call(SCI_SETREADONLY, 1);
    c.Call(SCI_SETUNDOCOLLECTION, 0);
    c.Call(SCI_SETMOUSEDWELLTIME, SC_TIME_FOREVER);
    const std::pair<Out, COLORREF> outColors[] = {
        {Out::Normal, Theme::Text}, {Out::Error, Theme::Error}, {Out::Warning, Theme::Warning},
        {Out::Info, Theme::Muted}, {Out::Success, Theme::Success}, {Out::Command, Theme::Dim},
        {Out::Input, Theme::Link}};
    for (auto [style, color] : outColors) {
        c.Call(SCI_STYLESETFORE, static_cast<int>(style), color);
        c.Call(SCI_STYLESETITALIC, static_cast<int>(style), 0);
        c.Call(SCI_STYLESETBOLD, static_cast<int>(style), style == Out::Success);
    }
    static const uint32_t ansi[16] = {0x7A7E85, 0xF75464, 0x5FB865, 0xE0B44A, 0x56A8F5, 0xC77DBB, 0x2AACB8, 0xBCBEC4,
                                      0x9DA0A8, 0xFF8A95, 0x8BD48F, 0xF2D27A, 0x8CC4FA, 0xE0A6D8, 0x6FD3DE, 0xFFFFFF};
    for (int i = 0; i < 16; i++) {
        c.Call(SCI_STYLESETFORE, Terminal::kStyleBase + i, Hex(ansi[i]));
        c.Call(SCI_STYLESETITALIC, Terminal::kStyleBase + i, 0);
    }
    c.Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, static_cast<sptr_t>(Theme::Bright) | (0xFFll << 24));
    SetWindowSubclass(c.hwnd(), ConsoleProc, 3, reinterpret_cast<DWORD_PTR>(this));
    ShowWindow(c.hwnd(), SW_SHOW);

    regsView_ = std::make_unique<Editor>(hwnd_, settings_->fontSize);
    memView_ = std::make_unique<Editor>(hwnd_, settings_->fontSize);
    SetupPanelView(*regsView_);
    SetupPanelView(*memView_);
    memExpr_ = CreateWindowExW(0, L"EDIT", L"rsp", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(ID_MEM_EXPR), inst_, nullptr);
    SendMessageW(memExpr_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"адрес: rsp, msg, rbx+8, [rsp]… Enter"));
    SetWindowSubclass(memExpr_, InputProc, 2, reinterpret_cast<DWORD_PTR>(this));

    CreateFonts();
    if (memExpr_) {
        SendMessageW(memExpr_, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        SendMessageW(memExpr_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
    }
    BuildMenus();

    tools_ = {
        {ID_MENU_FILE, L"Файл", 0, true, false},
        {ID_MENU_EDIT, L"Правка", 0, true, false},
        {ID_MENU_VIEW, L"Вид", 0, true, false},
        {ID_MENU_RUN, L"Запуск", 0, true, false},
        {ID_BUILD, L"Собрать", 1, false, false},
        {ID_RUN, L"Запустить", 2, false, false, 2},
        {ID_DEBUG, L"Отладка", 6, false, false, 2},
        {ID_DEBUG, L"Продолжить", 2, false, false, 1},
        {ID_PAUSE, L"Пауза", 7, false, false, 1},
        {ID_STEP_OVER, L"Шаг", 8, false, false, 1},
        {ID_STEP_INTO, L"Войти", 9, false, false, 1},
        {ID_STEP_OUT, L"Выйти", 10, false, false, 1},
        {ID_STOP, L"Стоп", 3, false, false},
        {ID_SETTINGS, L"Настройки", 4, false, true},
    };
}

void MainWindow::SetupPanelView(Editor& v) {
    v.Call(SCI_SETMARGINWIDTHN, 0, 0);
    v.Call(SCI_SETMARGINWIDTHN, 1, 0);
    v.Call(SCI_SETMARGINWIDTHN, 2, 4);
    v.Call(SCI_SETCARETLINEVISIBLEALWAYS, 0);
    v.Call(SCI_SETCARETLINEVISIBLE, 0);
    v.Call(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    v.Call(SCI_SETREADONLY, 1);
    v.Call(SCI_SETUNDOCOLLECTION, 0);
    v.Call(SCI_SETMOUSEDWELLTIME, SC_TIME_FOREVER);
    v.Call(SCI_SETHSCROLLBAR, 0);
    const std::pair<int, COLORREF> colors[] = {
        {P_TEXT, Theme::Text}, {P_LABEL, Hex(0xC77DBB)}, {P_CHANGED, Hex(0xFF8F6B)}, {P_HEADER, Theme::Muted},
        {P_SYMBOL, Theme::Link}, {P_FLAG_ON, Theme::Success}, {P_DIM, Hex(0x5F6369)}, {P_ERROR, Theme::Error},
        {P_ADDR, Theme::Muted}};
    for (auto [style, color] : colors) {
        v.Call(SCI_STYLESETFORE, style, color);
        v.Call(SCI_STYLESETITALIC, style, 0);
        v.Call(SCI_STYLESETBOLD, style, style == P_HEADER || style == P_CHANGED);
        v.Call(SCI_STYLESETSIZE, style, std::max(6, settings_->fontSize - 1));
    }
}

void MainWindow::BuildMenus() {
    fileMenu_ = CreatePopupMenu();
    recentMenu_ = CreatePopupMenu();
    AppendMenuW(fileMenu_, MF_STRING, ID_NEW, L"Новый файл\tCtrl+N");
    AppendMenuW(fileMenu_, MF_STRING, ID_NEW_PROJECT, L"Новый проект…");
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu_, MF_STRING, ID_OPEN_FILE, L"Открыть файл…\tCtrl+O");
    AppendMenuW(fileMenu_, MF_STRING, ID_OPEN_FOLDER, L"Открыть папку…\tCtrl+Shift+O");
    AppendMenuW(fileMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(recentMenu_), L"Недавние проекты");
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu_, MF_STRING, ID_SAVE, L"Сохранить\tCtrl+S");
    AppendMenuW(fileMenu_, MF_STRING, ID_SAVE_AS, L"Сохранить как…\tCtrl+Shift+S");
    AppendMenuW(fileMenu_, MF_STRING, ID_SAVE_ALL, L"Сохранить всё");
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu_, MF_STRING, ID_CLOSE_TAB, L"Закрыть вкладку\tCtrl+W");
    AppendMenuW(fileMenu_, MF_STRING, ID_CLOSE_PROJECT, L"Закрыть проект");
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu_, MF_STRING, ID_EXIT, L"Выход");

    editMenu_ = CreatePopupMenu();
    AppendMenuW(editMenu_, MF_STRING, ID_UNDO, L"Отменить\tCtrl+Z");
    AppendMenuW(editMenu_, MF_STRING, ID_REDO, L"Повторить\tCtrl+Y");
    AppendMenuW(editMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(editMenu_, MF_STRING, ID_FIND, L"Найти…\tCtrl+F");
    AppendMenuW(editMenu_, MF_STRING, ID_REPLACE, L"Заменить…\tCtrl+H");
    AppendMenuW(editMenu_, MF_STRING, ID_FIND_NEXT, L"Найти далее\tF3");
    AppendMenuW(editMenu_, MF_STRING, ID_FIND_PREV, L"Найти назад\tShift+F3");
    AppendMenuW(editMenu_, MF_STRING, ID_GOTO, L"Перейти к строке…\tCtrl+G");
    AppendMenuW(editMenu_, MF_STRING, ID_COMPLETE, L"Автодополнение\tCtrl+Space");
    AppendMenuW(editMenu_, MF_STRING, ID_SHOW_HINT, L"Подсказка по инструкции\tCtrl+Shift+Space");
    AppendMenuW(editMenu_, MF_STRING, ID_GOTO_DEF, L"Перейти к определению\tF12 / Ctrl+клик");
    AppendMenuW(editMenu_, MF_STRING, ID_FIND_USAGES, L"Найти использования\tShift+F12");
    AppendMenuW(editMenu_, MF_STRING, ID_RENAME, L"Переименовать…\tF2");
    AppendMenuW(editMenu_, MF_STRING, ID_NAV_BACK, L"Назад\tAlt+←");
    AppendMenuW(editMenu_, MF_STRING, ID_NAV_FORWARD, L"Вперёд\tAlt+→");
    AppendMenuW(editMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(editMenu_, MF_STRING, ID_COMMENT, L"Закомментировать / раскомментировать\tCtrl+/");
    AppendMenuW(editMenu_, MF_STRING, ID_DUPLICATE, L"Дублировать строку\tCtrl+D");

    viewMenu_ = CreatePopupMenu();
    AppendMenuW(viewMenu_, MF_STRING, ID_ZOOM_IN, L"Увеличить шрифт\tCtrl+=");
    AppendMenuW(viewMenu_, MF_STRING, ID_ZOOM_OUT, L"Уменьшить шрифт\tCtrl+-");
    AppendMenuW(viewMenu_, MF_STRING, ID_ZOOM_RESET, L"Сбросить размер шрифта\tCtrl+0");
    AppendMenuW(viewMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(viewMenu_, MF_STRING, ID_NEXT_TAB, L"Следующая вкладка\tCtrl+Tab");
    AppendMenuW(viewMenu_, MF_STRING, ID_PREV_TAB, L"Предыдущая вкладка\tCtrl+Shift+Tab");

    AppendMenuW(viewMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(viewMenu_, MF_STRING, ID_DEBUG_PANEL, L"Панель отладки");

    runMenu_ = CreatePopupMenu();
    AppendMenuW(runMenu_, MF_STRING, ID_BUILD, L"Собрать\tCtrl+F9");
    AppendMenuW(runMenu_, MF_STRING, ID_REBUILD, L"Пересобрать всё\tCtrl+Shift+F9");
    AppendMenuW(runMenu_, MF_STRING, ID_RUN, L"Запустить без отладки\tCtrl+F5 / Shift+F10");
    AppendMenuW(runMenu_, MF_STRING, ID_DEBUG, L"Отладка / продолжить\tF5");
    AppendMenuW(runMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(runMenu_, MF_STRING, ID_STEP_OVER, L"Шаг с обходом\tF10");
    AppendMenuW(runMenu_, MF_STRING, ID_STEP_INTO, L"Шаг с заходом\tF11");
    AppendMenuW(runMenu_, MF_STRING, ID_STEP_OUT, L"Шаг с выходом\tShift+F11");
    AppendMenuW(runMenu_, MF_STRING, ID_PAUSE, L"Пауза");
    AppendMenuW(runMenu_, MF_STRING, ID_STOP, L"Остановить\tShift+F5");
    AppendMenuW(runMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(runMenu_, MF_STRING, ID_TOGGLE_BP, L"Точка останова\tF9");
    AppendMenuW(runMenu_, MF_STRING, ID_CLEAR_BPS, L"Удалить все точки останова");
    AppendMenuW(runMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(runMenu_, MF_STRING, ID_CLEAR_CONSOLE, L"Очистить консоль");
    AppendMenuW(runMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(runMenu_, MF_STRING, ID_RESET_MAIN, L"Сбросить главный файл проекта");
    AppendMenuW(runMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(runMenu_, MF_STRING, ID_SETTINGS, L"Настройки…\tCtrl+Alt+S");
}

bool MainWindow::PreTranslate(MSG& msg) {
    if (findDialog_ && IsDialogMessageW(findDialog_, &msg)) return true;
    if (msg.hwnd == hwnd_ || IsChild(hwnd_, msg.hwnd)) {
        if (TranslateAcceleratorW(hwnd_, accel_, &msg)) return true;
    }
    return false;
}

// ---- window procedure -------------------------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (id == 2) self->RenderMemory();
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == findMsg_ && findMsg_) {
        OnFindMessage(reinterpret_cast<FINDREPLACEW*>(lp));
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        CreateChildren();
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        CreateFonts();
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        Layout();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd_) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd_, &p);
            if (drag_ == Drag::Tree || Contains(rcSplitV_, p.x, p.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            if (drag_ == Drag::Console || Contains(rcSplitH_, p.x, p.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            if (drag_ == Drag::Debug || (DebugPanelVisible() && Contains(rcSplitD_, p.x, p.y))) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            if (hoverLink_ >= 0 || hoverClear_ || hoverRefresh_) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        hoverTool_ = hoverTab_ = hoverClose_ = hoverLink_ = -1;
        hoverClear_ = hoverRefresh_ = false;
        InvalidateChrome();
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (Contains(rcSplitV_, x, y)) drag_ = Drag::Tree;
        else if (Contains(rcSplitH_, x, y)) drag_ = Drag::Console;
        else if (DebugPanelVisible() && Contains(rcSplitD_, x, y)) drag_ = Drag::Debug;
        if (drag_ != Drag::None) SetCapture(hwnd_);
        else OnClick(x, y, false);
        return 0;
    }
    case WM_LBUTTONUP:
        if (drag_ != Drag::None) {
            drag_ = Drag::None;
            ReleaseCapture();
        }
        return 0;
    case WM_MBUTTONUP:
        OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &p);
        if (Contains(rcTabs_, p.x, p.y)) {
            tabScroll_ = std::max(0, tabScroll_ - GET_WHEEL_DELTA_WPARAM(wp) / 2);
            InvalidateRect(hwnd_, &rcTabs_, FALSE);
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_MEM_EXPR) {
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(hwnd_, &rcMemRow_, FALSE);
            return 0;
        }
        if (HIWORD(wp) == 0 || HIWORD(wp) == 1) OnCommand(LOWORD(wp));
        return 0;
    case WM_NOTIFY:
        OnNotify(reinterpret_cast<NMHDR*>(lp));
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, Theme::Bright);
        SetBkColor(dc, Theme::InputBg);
        return reinterpret_cast<LRESULT>(inputBrush_);
    }
    case WM_CTLCOLORSTATIC: {  // disabled edit
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, Theme::Muted);
        SetBkColor(dc, Theme::InputBg);
        return reinterpret_cast<LRESULT>(inputBrush_);
    }
    case WM_TIMER:
        if (wp == kConsoleFocusTimer) {
            KillTimer(hwnd_, wp);
            if (debugging_ && !DebugStopped() && termActive_) SetFocus(console_->hwnd());
            return 0;
        }
        if (wp >= kCheckTimerBase) {
            KillTimer(hwnd_, wp);
            if (Editor* ed = FindById(static_cast<int>(wp - kCheckTimerBase))) RequestCheck(ed);
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (wp) RefreshTree();
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t path[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path)))) OpenPath(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_APP_OUTPUT: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        Print(*text, static_cast<Out>(wp));
        return 0;
    }
    case WM_APP_BUILD_DONE: {
        std::unique_ptr<BuildDone> done(reinterpret_cast<BuildDone*>(lp));
        OnBuildDone(&done->result, static_cast<After>(done->after), done->src);
        return 0;
    }
    case WM_APP_PROC_EXIT:
        OnProgramExit(static_cast<DWORD>(wp), static_cast<DWORD>(lp));
        return 0;
    case WM_APP_PTY_OUTPUT: {
        std::unique_ptr<std::string> data(reinterpret_cast<std::string*>(lp));
        if (wp != consoleSession_ || !termActive_) return 0;
        term_.Feed(data->data(), data->size());
        SendToProgram(term_.TakeResponse());
        if (!renderPending_) {  // render once per batch of queued output
            renderPending_ = true;
            PostMessageW(hwnd_, WM_APP_PTY_RENDER, 0, 0);
        }
        return 0;
    }
    case WM_APP_PTY_RENDER:
        renderPending_ = false;
        if (termActive_) RenderTerminal();
        return 0;
    case WM_APP_PTY_DONE:
        if (static_cast<DWORD>(lp) == consoleSession_) OnConsoleDone(static_cast<DWORD>(wp));
        return 0;
    case WM_DEBUG_STOPPED:
        OnDebugStopped(std::unique_ptr<DebugStop>(reinterpret_cast<DebugStop*>(lp)));
        return 0;
    case WM_DEBUG_EXITED:
        OnDebugExited(static_cast<DWORD>(wp));
        return 0;
    case WM_DEBUG_LOG: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        Print(*text, static_cast<Out>(wp));
        return 0;
    }
    case WM_APP_CHECK_DONE: {
        std::unique_ptr<CheckDone> done(reinterpret_cast<CheckDone*>(lp));
        Editor* ed = FindById(done->editorId);
        if (ed && ed->version() == done->version) {
            ed->SetDiagnostics(done->diagnostics);
            InvalidateChrome();
        }
        return 0;
    }
    case WM_APP_TOOL_STATUS: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        toolStatus_ = *text;
        toolStatusWarn_ = wp != 0;
        InvalidateRect(hwnd_, &rcStatus_, FALSE);
        return 0;
    }
    case WM_CLOSE: {
        SaveProjectSession();
        for (int i = 0; i < (int)editors_.size(); i++) {
            if (!ConfirmClose(i)) return 0;
        }
        StopProgram();
        debugger_.reset();  // terminates the debuggee and joins the debugger thread
        if (pty_) ClosePseudoConsole(pty_);
        pty_ = nullptr;
        if (ptyReader_.joinable()) ptyReader_.detach();
        WINDOWPLACEMENT wpl{sizeof(wpl)};
        GetWindowPlacement(hwnd_, &wpl);
        settings_->maximized = wpl.showCmd == SW_SHOWMAXIMIZED;
        const RECT& r = wpl.rcNormalPosition;
        settings_->window = {r.left, r.top, r.right - r.left, r.bottom - r.top};
        settings_->Save();
        {
            std::lock_guard<std::mutex> lock(checkMutex_);
            checkStop_ = true;
        }
        checkCv_.notify_all();
        if (checkThread_.joinable()) checkThread_.join();
        editors_.clear();
        DestroyWindow(hwnd_);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---- layout & painting ------------------------------------------------------------------------------------------

void MainWindow::Layout() {
    if (!tree_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    int tb = S(38), sb = S(24), header = S(28), split = S(4), tabsH = S(32), inputH = S(32);
    int treeW = std::clamp(S(settings_->treeWidth), S(120), std::max(S(120), W - S(300)));
    int bodyTop = tb, bodyBottom = H - sb;
    int consoleH = std::clamp(S(settings_->consoleHeight), S(90), std::max(S(90), bodyBottom - bodyTop - S(140)));

    rcToolbar_ = {0, 0, W, tb};
    rcStatus_ = {0, bodyBottom, W, H};
    rcTreeHeader_ = {0, bodyTop, treeW, bodyTop + header};
    rcRefresh_ = {treeW - S(30), bodyTop + S(2), treeW - S(6), bodyTop + header - S(2)};
    rcSplitV_ = {treeW, bodyTop, treeW + split, bodyBottom};
    int x0 = treeW + split;

    // Debug panel on the right.
    bool panel = DebugPanelVisible();
    int debugW = panel ? std::clamp(S(settings_->debugWidth), S(220), std::max(S(220), W - x0 - S(300))) : 0;
    int x1 = panel ? W - debugW - split : W;  // right edge of the editor column
    rcSplitD_ = panel ? RECT{x1, bodyTop, x1 + split, bodyBottom} : RECT{};
    rcDebug_ = panel ? RECT{x1 + split, bodyTop, W, bodyBottom} : RECT{};
    int regsBottom = rcDebug_.top + header + (bodyBottom - bodyTop - header) * 58 / 100;
    rcDebugHeader_ = {rcDebug_.left, bodyTop, W, bodyTop + header};
    rcMemHeader_ = {rcDebug_.left, regsBottom, W, regsBottom + header};
    rcMemRow_ = {rcDebug_.left, rcMemHeader_.bottom, W, rcMemHeader_.bottom + inputH};

    rcSplitH_ = {x0, bodyBottom - consoleH - split, x1, bodyBottom - consoleH};
    rcTabs_ = {x0, bodyTop, x1, bodyTop + tabsH};
    rcEditor_ = {x0, bodyTop + tabsH, x1, rcSplitH_.top};
    rcConsoleHeader_ = {x0, rcSplitH_.bottom, x1, rcSplitH_.bottom + header};
    rcClear_ = {x1 - S(90), rcConsoleHeader_.top, x1 - S(8), rcConsoleHeader_.bottom};
    W = x1;  // the console ends where the debug panel starts

    for (HWND h : {regsView_->hwnd(), memView_->hwnd(), memExpr_}) ShowWindow(h, panel ? SW_SHOW : SW_HIDE);
    if (panel) {
        SetWindowPos(regsView_->hwnd(), nullptr, rcDebug_.left, rcDebugHeader_.bottom, debugW,
                     rcMemHeader_.top - rcDebugHeader_.bottom, SWP_NOZORDER);
        SetWindowPos(memExpr_, nullptr, rcDebug_.left + S(8), rcMemRow_.top + S(5), debugW - S(16), inputH - S(10),
                     SWP_NOZORDER);
        SetWindowPos(memView_->hwnd(), nullptr, rcDebug_.left, rcMemRow_.bottom, debugW, bodyBottom - rcMemRow_.bottom,
                     SWP_NOZORDER);
    }

    HDWP dwp = BeginDeferWindowPos(4 + (int)editors_.size());
    dwp = DeferWindowPos(dwp, tree_, nullptr, 0, rcTreeHeader_.bottom, treeW, bodyBottom - rcTreeHeader_.bottom, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, console_->hwnd(), nullptr, x0, rcConsoleHeader_.bottom, W - x0,
                         bodyBottom - rcConsoleHeader_.bottom, SWP_NOZORDER);
    for (auto& ed : editors_) {
        dwp = DeferWindowPos(dwp, ed->hwnd(), nullptr, rcEditor_.left, rcEditor_.top, rcEditor_.right - rcEditor_.left,
                             rcEditor_.bottom - rcEditor_.top, SWP_NOZORDER);
    }
    EndDeferWindowPos(dwp);

    if (termActive_ && pty_) {  // keep the pseudoconsole the size of the console view
        auto [cols, rows] = ConsoleGridSize();
        if (cols != term_.cols() || rows != term_.rows()) {
            ResizePseudoConsole(pty_, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
            term_.Resize(cols, rows);
            RenderTerminal();
        }
    }

    // toolbar item rectangles
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, uiFont_);
    int x = S(6), right = rc.right - S(6);
    for (size_t i = 0; i < tools_.size(); i++) {
        ToolItem& t = tools_[i];
        if ((t.show == 1 && !debugging_) || (t.show == 2 && debugging_)) {
            t.rc = {};
            continue;
        }
        int w = TextWidth(dc, t.text) + S(t.icon ? 36 : 20);
        if (t.right) {
            t.rc = {right - w, S(5), right, tb - S(5)};
            right -= w + S(4);
        } else {
            if (i > 0 && tools_[i - 1].menu && !t.menu) x += S(14);  // gap after the menus
            t.rc = {x, S(5), x + w, tb - S(5)};
            x += w + S(2);
        }
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::InvalidateChrome() {
    InvalidateRect(hwnd_, &rcToolbar_, FALSE);
    InvalidateRect(hwnd_, &rcTabs_, FALSE);
    InvalidateRect(hwnd_, &rcStatus_, FALSE);
    InvalidateRect(hwnd_, &rcTreeHeader_, FALSE);
    InvalidateRect(hwnd_, &rcConsoleHeader_, FALSE);
    InvalidateRect(hwnd_, &rcDebugHeader_, FALSE);
    InvalidateRect(hwnd_, &rcMemHeader_, FALSE);
    if (editors_.empty()) InvalidateRect(hwnd_, &rcEditor_, FALSE);
}

void MainWindow::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    FillRectColor(dc, rc, Theme::PanelBg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, uiFont_);

    PaintToolbar(dc);
    FillRectColor(dc, {0, rcToolbar_.bottom - 1, rc.right, rcToolbar_.bottom}, Theme::Border);

    // project header
    SelectObject(dc, uiBold_);
    RECT th = rcTreeHeader_;
    th.left += S(12);
    DrawTextColor(dc, L"ПРОЕКТ", th, Theme::Muted, DT_SINGLELINE | DT_VCENTER);
    if (!projectRoot_.empty()) {
        if (hoverRefresh_) FillRectColor(dc, rcRefresh_, Theme::Hover);
        DrawIcon(dc, 5, rcRefresh_, true);
    } else {
        SelectObject(dc, uiFont_);
        RECT hint{S(12), rcTreeHeader_.bottom + S(10), rcTreeHeader_.right - S(8), rcTreeHeader_.bottom + S(90)};
        DrawTextColor(dc, L"Проект не открыт.\nCtrl+Shift+O — открыть папку.", hint, Theme::Dim, DT_WORDBREAK);
    }

    // splitters
    FillRectColor(dc, rcSplitV_, Theme::EditorBg);
    FillRectColor(dc, {rcSplitV_.left + (rcSplitV_.right - rcSplitV_.left) / 2, rcSplitV_.top,
                       rcSplitV_.left + (rcSplitV_.right - rcSplitV_.left) / 2 + 1, rcSplitV_.bottom}, Theme::Border);
    FillRectColor(dc, rcSplitH_, Theme::EditorBg);
    FillRectColor(dc, {rcSplitH_.left, rcSplitH_.top + 1, rcSplitH_.right, rcSplitH_.top + 2}, Theme::Border);

    PaintTabs(dc);
    if (editors_.empty()) PaintWelcome(dc);

    if (DebugPanelVisible()) {
        FillRectColor(dc, rcSplitD_, Theme::EditorBg);
        int mid = (rcSplitD_.left + rcSplitD_.right) / 2;
        FillRectColor(dc, {mid, rcSplitD_.top, mid + 1, rcSplitD_.bottom}, Theme::Border);
        SelectObject(dc, uiBold_);
        RECT h = rcDebugHeader_;
        h.left += S(12);
        DrawTextColor(dc, L"ОТЛАДКА", h, Theme::Muted, DT_SINGLELINE | DT_VCENTER);
        RECT m = rcMemHeader_;
        m.left += S(12);
        DrawTextColor(dc, L"ПАМЯТЬ", m, Theme::Muted, DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, uiFont_);
        RECT st = rcDebugHeader_;
        st.left += S(84);
        st.right -= S(10);
        DrawTextColor(dc, debugStatus_, st, debugStatusColor_, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
        RECT frame;
        GetWindowRect(memExpr_, &frame);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&frame), 2);
        InflateRect(&frame, 1, 1);
        SetDCBrushColor(dc, GetFocus() == memExpr_ ? Theme::Accent : Theme::Border);
        FrameRect(dc, &frame, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    // console header and input row
    SelectObject(dc, uiBold_);
    RECT ch = rcConsoleHeader_;
    ch.left += S(12);
    DrawTextColor(dc, L"КОНСОЛЬ", ch, Theme::Muted, DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, uiFont_);
    if (debugging_) {
        RECT run = ch;
        run.left += S(80);
        bool stopped = DebugStopped();
        DrawTextColor(dc, stopped ? L"● отладка: остановлено" : L"● отладка: выполняется", run,
                      stopped ? Theme::Warning : Theme::Success, DT_SINGLELINE | DT_VCENTER);
    } else if (process_) {
        RECT run = ch;
        run.left += S(80);
        DrawTextColor(dc, L"● программа выполняется — ввод прямо в консоли, Ctrl+Z и Enter — конец ввода", run,
                      Theme::Success, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else if (building_) {
        RECT run = ch;
        run.left += S(80);
        DrawTextColor(dc, L"● сборка…", run, Theme::Warning, DT_SINGLELINE | DT_VCENTER);
    }
    DrawTextColor(dc, L"Очистить", rcClear_, hoverClear_ ? Theme::Bright : Theme::Muted,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    PaintStatus(dc);
}

void MainWindow::DrawIcon(HDC dc, int icon, const RECT& rc, bool enabled) {
    int s = S(16);
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    int x = cx - s / 2, y = cy - s / 2;
    auto P = [&](double fx, double fy) { return POINT{x + static_cast<int>(fx * s / 16.0), y + static_cast<int>(fy * s / 16.0)}; };
    auto dim = [&](COLORREF c) {
        if (enabled) return c;
        return RGB((GetRValue(c) + 2 * GetRValue(Theme::PanelBg)) / 3, (GetGValue(c) + 2 * GetGValue(Theme::PanelBg)) / 3,
                   (GetBValue(c) + 2 * GetBValue(Theme::PanelBg)) / 3);
    };
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    switch (icon) {
    case 1: {  // hammer
        SetDCBrushColor(dc, dim(Hex(0x8FB3E0)));
        POINT handle[] = {P(3, 14), P(4.5, 15.5), P(11, 9), P(9.5, 7.5)};
        Polygon(dc, handle, 4);
        POINT head[] = {P(6, 4), P(10, 0.5), P(15.5, 6), P(12, 9.5)};
        Polygon(dc, head, 4);
        break;
    }
    case 2: {  // run
        SetDCBrushColor(dc, dim(Theme::Success));
        POINT tri[] = {P(4, 2), P(14, 8), P(4, 14)};
        Polygon(dc, tri, 3);
        break;
    }
    case 3:  // stop
        SetDCBrushColor(dc, dim(Theme::Error));
        RoundRect(dc, P(3, 3).x, P(3, 3).y, P(13, 13).x, P(13, 13).y, S(3), S(3));
        break;
    case 4: {  // gear
        SetDCBrushColor(dc, dim(Theme::Muted));
        for (int i = 0; i < 8; i++) {
            double a = i * 3.14159265 / 4;
            int tx = cx + static_cast<int>(std::cos(a) * s * 0.40), ty = cy + static_cast<int>(std::sin(a) * s * 0.40);
            int r = std::max(2, s / 8);
            Ellipse(dc, tx - r, ty - r, tx + r + 1, ty + r + 1);
        }
        Ellipse(dc, cx - s * 3 / 8, cy - s * 3 / 8, cx + s * 3 / 8 + 1, cy + s * 3 / 8 + 1);
        SetDCBrushColor(dc, Theme::PanelBg);
        Ellipse(dc, cx - s / 6, cy - s / 6, cx + s / 6 + 1, cy + s / 6 + 1);
        break;
    }
    case 6: {  // bug
        COLORREF c = dim(Hex(0xE5A050));
        SetDCBrushColor(dc, c);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), c);
        SelectObject(dc, pen);
        for (double ly : {6.5, 9.5, 12.5}) {
            MoveToEx(dc, P(2, ly).x, P(2, ly).y, nullptr);
            LineTo(dc, P(14, ly).x, P(14, ly).y);
        }
        SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, P(4.5, 4).x, P(4.5, 4).y, P(11.5, 15).x, P(11.5, 15).y);
        Ellipse(dc, P(5.5, 1).x, P(5.5, 1).y, P(10.5, 5.5).x, P(10.5, 5.5).y);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        break;
    }
    case 7:  // pause
        SetDCBrushColor(dc, dim(Theme::Link));
        RoundRect(dc, P(3.5, 3).x, P(3.5, 3).y, P(7, 13).x, P(7, 13).y, S(2), S(2));
        RoundRect(dc, P(9, 3).x, P(9, 3).y, P(12.5, 13).x, P(12.5, 13).y, S(2), S(2));
        break;
    case 8:    // step over
    case 9:    // step into
    case 10: { // step out
        COLORREF c = dim(Theme::Link);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2) * 3 / 4 + 1), c);
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (icon == 8) {
            Arc(dc, P(2, 3).x, P(2, 3).y, P(14, 13).x, P(14, 13).y, P(14, 8).x, P(14, 8).y, P(2, 8).x, P(2, 8).y);
        } else {
            MoveToEx(dc, P(8, 1).x, P(8, 1).y, nullptr);
            LineTo(dc, P(8, 9).x, P(8, 9).y);
        }
        SelectObject(dc, GetStockObject(NULL_PEN));
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, c);
        if (icon == 8) {
            POINT tri[] = {P(11, 6.5), P(16, 6.5), P(13.5, 10.5)};
            Polygon(dc, tri, 3);
        } else if (icon == 9) {
            POINT tri[] = {P(4.5, 7), P(11.5, 7), P(8, 11)};
            Polygon(dc, tri, 3);
        } else {
            POINT tri[] = {P(4.5, 4), P(11.5, 4), P(8, 0)};
            Polygon(dc, tri, 3);
        }
        Ellipse(dc, P(6, 12).x, P(6, 12).y, P(10.5, 16).x, P(10.5, 16).y);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        break;
    }
    case 5: {  // refresh: glyph from the Windows icon font
        HFONT f = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
        HGDIOBJ oldFont = SelectObject(dc, f);
        DrawTextColor(dc, L"", rc, Theme::Muted, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SelectObject(dc, oldFont);
        DeleteObject(f);
        break;
    }
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void MainWindow::PaintToolbar(HDC dc) {
    bool stopped = DebugStopped();
    for (size_t i = 0; i < tools_.size(); i++) {
        const ToolItem& t = tools_[i];
        if (t.rc.right <= t.rc.left) continue;  // hidden
        bool enabled = true;
        switch (t.id) {
        case ID_BUILD:
        case ID_RUN: enabled = !building_; break;
        case ID_DEBUG: enabled = debugging_ ? stopped : !building_; break;
        case ID_STEP_OVER:
        case ID_STEP_INTO:
        case ID_STEP_OUT: enabled = stopped; break;
        case ID_PAUSE: enabled = debugging_ && !stopped; break;
        case ID_STOP: enabled = process_ != nullptr || debugging_; break;
        }
        if ((int)i == hoverTool_ && enabled) {
            SetDCBrushColor(dc, Theme::Hover);
            SelectObject(dc, GetStockObject(DC_BRUSH));
            SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, t.rc.left, t.rc.top, t.rc.right, t.rc.bottom, S(6), S(6));
        }
        RECT tr = t.rc;
        if (t.icon) {
            RECT ir{tr.left + S(6), tr.top, tr.left + S(26), tr.bottom};
            DrawIcon(dc, t.icon, ir, enabled);
            tr.left += S(28);
            DrawTextColor(dc, t.text, tr, enabled ? Theme::Text : Theme::Dim, DT_SINGLELINE | DT_VCENTER);
        } else {
            DrawTextColor(dc, t.text, tr, Theme::Text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        if (i + 1 < tools_.size() && t.menu && !tools_[i + 1].menu) {
            int sx = t.rc.right + S(8);
            FillRectColor(dc, {sx, t.rc.top + S(4), sx + 1, t.rc.bottom - S(4)}, Theme::Border);
        }
    }
}

void MainWindow::PaintTabs(HDC dc) {
    FillRectColor(dc, rcTabs_, Theme::PanelBg);
    FillRectColor(dc, {rcTabs_.left, rcTabs_.bottom - 1, rcTabs_.right, rcTabs_.bottom}, Theme::Border);
    tabRects_.assign(editors_.size(), RECT{});
    closeRects_.assign(editors_.size(), RECT{});
    if (editors_.empty()) return;

    std::vector<int> widths;
    int total = 0;
    SelectObject(dc, uiFont_);
    for (auto& ed : editors_) {
        std::wstring title = (ed->Modified() ? L"● " : L"") + ed->Title();
        int w = TextWidth(dc, title) + S(48);
        widths.push_back(w);
        total += w;
    }
    int avail = rcTabs_.right - rcTabs_.left;
    if (ensureTabVisible_ && active_ >= 0) {
        int start = 0;
        for (int i = 0; i < active_; i++) start += widths[i];
        if (start < tabScroll_) tabScroll_ = start;
        if (start + widths[active_] > tabScroll_ + avail) tabScroll_ = start + widths[active_] - avail;
        ensureTabVisible_ = false;
    }
    tabScroll_ = std::clamp(tabScroll_, 0, std::max(0, total - avail));

    HRGN clip = CreateRectRgn(rcTabs_.left, rcTabs_.top, rcTabs_.right, rcTabs_.bottom);
    SelectClipRgn(dc, clip);
    int x = rcTabs_.left - tabScroll_;
    for (size_t i = 0; i < editors_.size(); i++) {
        Editor& ed = *editors_[i];
        RECT r{x, rcTabs_.top, x + widths[i], rcTabs_.bottom};
        tabRects_[i] = r;
        bool active = (int)i == active_;
        RECT body = r;
        if (active) body.bottom += 0;
        else body.bottom -= 1;
        FillRectColor(dc, body, active ? Theme::EditorBg : ((int)i == hoverTab_ ? Theme::Hover : Theme::PanelBg));
        if (active) FillRectColor(dc, {r.left, r.top, r.right, r.top + S(2)}, Theme::Accent);
        FillRectColor(dc, {r.right - 1, r.top + S(6), r.right, r.bottom - S(6)}, Theme::Border);

        bool errors = ed.Count(Severity::Error) > 0;
        bool isMain = !mainFile_.empty() && SamePath(ed.file(), mainFile_);
        std::wstring title = (ed.Modified() ? L"● " : L"") + ed.Title();
        RECT tr{r.left + S(12), r.top, r.right - S(28), r.bottom};
        SelectObject(dc, isMain ? uiBold_ : uiFont_);
        DrawTextColor(dc, title, tr, errors ? Theme::Error : (active ? Theme::Bright : Theme::Muted),
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT cr{r.right - S(26), r.top + S(8), r.right - S(8), r.bottom - S(6)};
        closeRects_[i] = cr;
        if ((int)i == hoverClose_) FillRectColor(dc, cr, Theme::Pressed);
        if (active || (int)i == hoverTab_) {
            SelectObject(dc, uiFont_);
            DrawTextColor(dc, L"✕", cr, (int)i == hoverClose_ ? Theme::Bright : Theme::Muted,
                          DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        x += widths[i];
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    SelectObject(dc, uiFont_);
}

void MainWindow::PaintWelcome(HDC dc) {
    FillRectColor(dc, rcEditor_, Theme::EditorBg);
    welcomeLinks_.clear();
    int x = rcEditor_.left + S(48);
    int y = rcEditor_.top + S(40);
    SelectObject(dc, titleFont_);
    DrawTextColor(dc, L"AsmEditor", {x, y, rcEditor_.right, y + S(44)}, Theme::Bright, DT_SINGLELINE);
    y += S(48);
    SelectObject(dc, uiFont_);
    DrawTextColor(dc, L"Редактор NASM: подсветка, проверка ошибок, сборка и запуск", {x, y, rcEditor_.right, y + S(20)},
                  Theme::Muted, DT_SINGLELINE);
    y += S(40);
    const wchar_t* hints[][2] = {
        {L"Ctrl+N", L"новый файл"},
        {L"Ctrl+O", L"открыть файл"},
        {L"Ctrl+Shift+O", L"открыть папку проекта"},
        {L"Ctrl+F5", L"собрать и запустить"},
        {L"F5", L"отладка (F9 — точка останова, F10/F11 — шаги)"},
        {L"Ctrl+F9", L"только собрать"},
        {L"Ctrl+Alt+S", L"настройки NASM и линкера"},
    };
    for (auto& h : hints) {
        DrawTextColor(dc, h[0], {x, y, x + S(110), y + S(20)}, Theme::Link, DT_SINGLELINE);
        DrawTextColor(dc, h[1], {x + S(120), y, rcEditor_.right, y + S(20)}, Theme::Text, DT_SINGLELINE);
        y += S(24);
    }
    if (!settings_->recent.empty()) {
        y += S(20);
        SelectObject(dc, uiBold_);
        DrawTextColor(dc, L"Недавние проекты", {x, y, rcEditor_.right, y + S(20)}, Theme::Muted, DT_SINGLELINE);
        y += S(28);
        SelectObject(dc, uiFont_);
        for (size_t i = 0; i < settings_->recent.size() && y < rcEditor_.bottom - S(24); i++) {
            const std::wstring& r = settings_->recent[i];
            RECT lr{x, y, x + TextWidth(dc, r), y + S(20)};
            DrawTextColor(dc, r, lr, (int)i == hoverLink_ ? Theme::Bright : Theme::Link, DT_SINGLELINE);
            welcomeLinks_.push_back({lr, r});
            y += S(24);
        }
    }
}

void MainWindow::PaintStatus(HDC dc) {
    FillRectColor(dc, rcStatus_, Theme::PanelBg);
    FillRectColor(dc, {rcStatus_.left, rcStatus_.top, rcStatus_.right, rcStatus_.top + 1}, Theme::Border);
    SelectObject(dc, uiFont_);
    RECT left = rcStatus_;
    left.left += S(12);
    DrawTextColor(dc, toolStatus_, left, toolStatusWarn_ ? Theme::Warning : Theme::Muted, DT_SINGLELINE | DT_VCENTER);

    std::vector<std::pair<std::wstring, COLORREF>> parts;
    if (Editor* ed = Active()) {
        int errors = ed->Count(Severity::Error), warnings = ed->Count(Severity::Warning);
        if (errors) parts.push_back({L"Ошибок: " + std::to_wstring(errors), Theme::Error});
        if (warnings) parts.push_back({L"Предупреждений: " + std::to_wstring(warnings), Theme::Warning});
        if (!mainFile_.empty()) parts.push_back({L"main: " + mainFile_.filename().wstring(), Theme::Muted});
        parts.push_back({L"Стр " + std::to_wstring(ed->CaretLine()) + L", Кол " + std::to_wstring(ed->CaretColumn()),
                         Theme::Muted});
        parts.push_back({ed->EncodingName(), Theme::Muted});
        parts.push_back({ed->EolName(), Theme::Muted});
    } else if (!mainFile_.empty()) {
        parts.push_back({L"main: " + mainFile_.filename().wstring(), Theme::Muted});
    }
    int x = rcStatus_.right - S(12);
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        int w = TextWidth(dc, it->first);
        DrawTextColor(dc, it->first, {x - w, rcStatus_.top, x, rcStatus_.bottom}, it->second, DT_SINGLELINE | DT_VCENTER);
        x -= w + S(20);
    }
}

void MainWindow::OnMouseMove(int x, int y) {
    if (drag_ == Drag::Tree) {
        settings_->treeWidth = std::max(80, MulDiv(x, 96, dpi_));
        Layout();
        return;
    }
    if (drag_ == Drag::Console) {
        settings_->consoleHeight = std::max(60, MulDiv(rcStatus_.top - y, 96, dpi_));
        Layout();
        return;
    }
    if (drag_ == Drag::Debug) {
        settings_->debugWidth = std::max(180, MulDiv(rcStatus_.right - x, 96, dpi_));
        Layout();
        return;
    }
    int tool = -1, tab = -1, close = -1, link = -1;
    for (size_t i = 0; i < tools_.size(); i++) if (Contains(tools_[i].rc, x, y)) tool = (int)i;
    for (size_t i = 0; i < tabRects_.size(); i++) {
        if (Contains(tabRects_[i], x, y) && Contains(rcTabs_, x, y)) tab = (int)i;
        if (Contains(closeRects_[i], x, y) && Contains(rcTabs_, x, y)) close = (int)i;
    }
    if (editors_.empty()) {
        for (size_t i = 0; i < welcomeLinks_.size(); i++) if (Contains(welcomeLinks_[i].first, x, y)) link = (int)i;
    }
    bool clear = Contains(rcClear_, x, y);
    bool refresh = !projectRoot_.empty() && Contains(rcRefresh_, x, y);
    if (tool != hoverTool_ || tab != hoverTab_ || close != hoverClose_ || link != hoverLink_ || clear != hoverClear_ ||
        refresh != hoverRefresh_) {
        hoverTool_ = tool;
        hoverTab_ = tab;
        hoverClose_ = close;
        hoverLink_ = link;
        hoverClear_ = clear;
        hoverRefresh_ = refresh;
        InvalidateChrome();
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
    TrackMouseEvent(&tme);
}

void MainWindow::OnClick(int x, int y, bool middle) {
    for (size_t i = 0; i < tabRects_.size(); i++) {
        if (!Contains(tabRects_[i], x, y) || !Contains(rcTabs_, x, y)) continue;
        if (middle || Contains(closeRects_[i], x, y)) CloseEditor((int)i);
        else Activate((int)i);
        return;
    }
    if (middle) return;
    for (const ToolItem& t : tools_) {
        if (!Contains(t.rc, x, y)) continue;
        if (t.menu) {
            HMENU menu = t.id == ID_MENU_FILE ? fileMenu_ : t.id == ID_MENU_EDIT ? editMenu_
                       : t.id == ID_MENU_VIEW ? viewMenu_ : runMenu_;
            if (menu == fileMenu_) {
                while (GetMenuItemCount(recentMenu_) > 0) DeleteMenu(recentMenu_, 0, MF_BYPOSITION);
                for (size_t i = 0; i < settings_->recent.size(); i++) {
                    AppendMenuW(recentMenu_, MF_STRING, ID_RECENT + i, settings_->recent[i].c_str());
                }
                if (settings_->recent.empty()) AppendMenuW(recentMenu_, MF_STRING | MF_GRAYED, 0, L"(пусто)");
            }
            EnableMenuItem(runMenu_, ID_STOP, process_ ? MF_ENABLED : MF_GRAYED);
            POINT p{t.rc.left, t.rc.bottom};
            ClientToScreen(hwnd_, &p);
            TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, hwnd_, nullptr);
        } else {
            OnCommand(t.id);
        }
        return;
    }
    if (Contains(rcClear_, x, y)) OnCommand(ID_CLEAR_CONSOLE);
    else if (!projectRoot_.empty() && Contains(rcRefresh_, x, y)) RefreshTree();
    else if (editors_.empty()) {
        for (auto& [r, path] : welcomeLinks_) {
            if (Contains(r, x, y)) {
                OpenPath(path);
                return;
            }
        }
    }
}

void MainWindow::UpdateTitle() {
    std::wstring title;
    if (!projectRoot_.empty()) title += projectRoot_.filename().wstring() + L" – ";
    if (Editor* ed = Active()) title += ed->Title() + L" – ";
    title += L"AsmEditor";
    SetWindowTextW(hwnd_, title.c_str());
}

// ---- commands ---------------------------------------------------------------------------------------------------

void MainWindow::OnCommand(int id) {
    Editor* ed = Active();
    if (id >= ID_RECENT && id < ID_RECENT + 100) {
        size_t i = id - ID_RECENT;
        if (i < settings_->recent.size()) OpenPath(settings_->recent[i]);
        return;
    }
    switch (id) {
    case ID_NEW:
        AddEditor(std::make_unique<Editor>(hwnd_, settings_->fontSize));
        break;
    case ID_NEW_PROJECT: NewProject(); break;
    case ID_OPEN_FILE: {
        std::vector<wchar_t> buf(32768, L'\0');
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"Ассемблер (*.asm;*.inc;*.s;*.nasm;*.mac)\0*.asm;*.inc;*.s;*.nasm;*.mac\0Все файлы\0*.*\0";
        ofn.lpstrFile = buf.data();
        ofn.nMaxFile = static_cast<DWORD>(buf.size());
        std::wstring initial = projectRoot_.wstring();
        ofn.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) break;
        fs::path first = buf.data();
        const wchar_t* p = buf.data() + wcslen(buf.data()) + 1;
        if (!*p) {
            OpenFile(first);
        } else {
            for (; *p; p += wcslen(p) + 1) OpenFile(first / p);
        }
        break;
    }
    case ID_OPEN_FOLDER:
        if (auto dir = PickFolder(L"Открыть папку проекта")) OpenProject(*dir);
        break;
    case ID_SAVE: Save(ed); break;
    case ID_SAVE_AS: SaveAs(ed); break;
    case ID_SAVE_ALL: SaveAll(); break;
    case ID_CLOSE_TAB: if (ed) CloseEditor(active_); break;
    case ID_CLOSE_PROJECT: CloseProject(); break;
    case ID_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case ID_UNDO: if (ed) ed->Call(SCI_UNDO); break;
    case ID_REDO: if (ed) ed->Call(SCI_REDO); break;
    case ID_FIND: ShowFind(false); break;
    case ID_REPLACE: ShowFind(true); break;
    case ID_FIND_NEXT:
    case ID_FIND_PREV:
        if (lastFind_.empty()) ShowFind(false);
        else FindNext(id == ID_FIND_NEXT);
        break;
    case ID_GOTO:
        if (ed) {
            if (auto s = InputBox(hwnd_, L"Перейти к строке", L"Номер строки:", std::to_wstring(ed->CaretLine()))) {
                int line = _wtoi(s->c_str());
                if (line > 0) ed->GotoLine(line);
            }
        }
        break;
    case ID_COMMENT: if (ed) ed->ToggleComment(); break;
    case ID_COMPLETE: if (ed) Complete(ed, true); break;
    case ID_SHOW_HINT: if (ed) ShowHint(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_GOTO_DEF: if (ed) GoToDefinition(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_FIND_USAGES: if (ed) FindUsages(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_RENAME: if (ed) Rename(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_NAV_BACK: Navigate(true); break;
    case ID_NAV_FORWARD: Navigate(false); break;
    case ID_DUPLICATE: if (ed) ed->Call(SCI_SELECTIONDUPLICATE); break;
    case ID_ZOOM_IN: ApplyFontSize(settings_->fontSize + 1); break;
    case ID_ZOOM_OUT: ApplyFontSize(settings_->fontSize - 1); break;
    case ID_ZOOM_RESET: ApplyFontSize(11); break;
    case ID_NEXT_TAB: if (!editors_.empty()) Activate((active_ + 1) % (int)editors_.size()); break;
    case ID_PREV_TAB: if (!editors_.empty()) Activate((active_ + (int)editors_.size() - 1) % (int)editors_.size()); break;
    case ID_BUILD: StartBuild(After::Nothing); break;
    case ID_REBUILD: StartBuild(After::Nothing, true); break;
    case ID_RUN: if (!debugging_) StartBuild(After::Run); break;
    case ID_DEBUG:
    case ID_STEP_OVER:
    case ID_STEP_INTO:
    case ID_STEP_OUT:
    case ID_PAUSE:
        DebugCommand(id);
        break;
    case ID_TOGGLE_BP:
        if (ed) ToggleBreakpoint(ed, ed->CaretLine());
        break;
    case ID_CLEAR_BPS:
        breakpoints_.clear();
        for (auto& e : editors_) {
            e->SetBreakpoints({});
            if (debugger_ && !e->file().empty()) debugger_->SetBreakpoints(e->file(), {});
        }
        break;
    case ID_DEBUG_PANEL:
        showDebugPanel_ = !showDebugPanel_;
        CheckMenuItem(viewMenu_, ID_DEBUG_PANEL, showDebugPanel_ ? MF_CHECKED : MF_UNCHECKED);
        Layout();
        break;
    case ID_STOP: StopProgram(); break;
    case ID_CLEAR_CONSOLE:
        console_->Call(SCI_SETREADONLY, 0);
        console_->Call(SCI_CLEARALL);
        console_->Call(SCI_SETREADONLY, 1);
        termStart_ = 0;
        if (termActive_) {
            term_.Commit();
            term_.TakeHistory();
            RenderTerminal();
        }
        break;
    case ID_RESET_MAIN: SetMainFile({}); break;
    case ID_SETTINGS:
        if (ShowSettingsDialog(hwnd_, *settings_)) {
            ApplyFontSize(settings_->fontSize);
            RefreshToolStatus();
            for (auto& e : editors_) {
                if (settings_->liveCheck) RequestCheck(e.get());
                else e->SetDiagnostics({});
            }
            InvalidateChrome();
        }
        break;
    case ID_TREE_NEW_FILE:
    case ID_TREE_NEW_FOLDER: {
        fs::path target = contextItem_ ? TreePath(contextItem_) : projectRoot_;
        fs::path dir = DirExists(target) ? target : target.parent_path();
        bool folder = id == ID_TREE_NEW_FOLDER;
        auto name = InputBox(hwnd_, folder ? L"Новая папка" : L"Новый файл", folder ? L"Имя папки:" : L"Имя файла:",
                             folder ? L"" : L"main.asm");
        if (!name || Trim(*name).empty()) break;
        std::wstring n = Trim(*name);
        std::error_code ec;
        if (folder) {
            fs::create_directories(dir / n, ec);
        } else {
            if (n.find(L'.') == std::wstring::npos) n += L".asm";
            fs::path p = dir / n;
            fs::create_directories(p.parent_path(), ec);
            if (!fs::exists(p)) std::ofstream(p, std::ios::binary).close();
            RefreshTree();
            OpenFile(p);
        }
        if (ec) ShowMessage(hwnd_, L"Не удалось создать: " + Widen(ec.message(), CP_ACP));
        RefreshTree();
        break;
    }
    case ID_TREE_OPEN:
        if (contextItem_) OpenFile(TreePath(contextItem_));
        break;
    case ID_TREE_EXCLUDE:
        if (contextItem_) {
            std::wstring key = Project::Key(TreePath(contextItem_));
            if (!excludedFromBuild_.erase(key)) excludedFromBuild_.insert(key);
            SaveProjectSession();
            RefreshTree();
        }
        break;
    case ID_TREE_MAIN:
        if (contextItem_) {
            fs::path p = TreePath(contextItem_);
            SetMainFile(SamePath(p, mainFile_) ? fs::path() : p);
        }
        break;
    case ID_TREE_RENAME: {
        if (!contextItem_) break;
        fs::path from = TreePath(contextItem_);
        auto name = InputBox(hwnd_, L"Переименовать", L"Новое имя:", from.filename().wstring());
        if (!name || Trim(*name).empty() || Trim(*name) == from.filename().wstring()) break;
        fs::path to = from.parent_path() / Trim(*name);
        if (!MoveFileW(from.c_str(), to.c_str())) {
            ShowMessage(hwnd_, L"Не удалось переименовать.");
            break;
        }
        for (auto& e : editors_) {
            if (SamePath(e->file(), from)) e->setFile(to);
            else if (IsUnder(e->file(), from)) e->setFile(to / e->file().lexically_relative(from));
        }
        if (SamePath(mainFile_, from)) SetMainFile(to);
        else if (IsUnder(mainFile_, from)) SetMainFile(to / mainFile_.lexically_relative(from));
        RefreshTree();
        InvalidateChrome();
        UpdateTitle();
        break;
    }
    case ID_TREE_DELETE: {
        if (!contextItem_) break;
        fs::path target = TreePath(contextItem_);
        std::wstring q = L"Переместить «" + target.filename().wstring() + L"» в корзину?";
        if (MessageBoxW(hwnd_, q.c_str(), L"Удаление", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
        std::wstring from = target.wstring();
        from.push_back(L'\0');  // double-null terminated list
        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd_;
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
            ShowMessage(hwnd_, L"Не удалось удалить.");
            break;
        }
        for (int i = (int)editors_.size() - 1; i >= 0; i--) {
            const fs::path& f = editors_[i]->file();
            if (SamePath(f, target) || IsUnder(f, target)) {
                editors_.erase(editors_.begin() + i);
                if (active_ >= (int)editors_.size()) active_ = (int)editors_.size() - 1;
            }
        }
        if (SamePath(mainFile_, target) || IsUnder(mainFile_, target)) SetMainFile({});
        Activate(active_);
        RefreshTree();
        break;
    }
    case ID_TREE_REVEAL:
        if (contextItem_) {
            std::wstring args = L"/select,\"" + TreePath(contextItem_).wstring() + L"\"";
            ShellExecuteW(hwnd_, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        break;
    case ID_REFRESH_TREE: RefreshTree(); break;
    }
}

void MainWindow::OnNotify(NMHDR* nm) {
    if (nm->hwndFrom == tree_) {
        switch (nm->code) {
        case NM_DBLCLK:
        case NM_RETURN: {
            HTREEITEM item = TreeView_GetSelection(tree_);
            fs::path p = TreePath(item);
            if (!p.empty() && !DirExists(p)) OpenFile(p);
            break;
        }
        case NM_RCLICK: {
            POINT pt;
            GetCursorPos(&pt);
            TVHITTESTINFO hit{};
            hit.pt = pt;
            ScreenToClient(tree_, &hit.pt);
            contextItem_ = TreeView_HitTest(tree_, &hit);
            if (contextItem_) TreeView_SelectItem(tree_, contextItem_);
            TreeContextMenu();
            break;
        }
        case TVN_GETINFOTIPW: {
            auto* tip = reinterpret_cast<NMTVGETINFOTIPW*>(nm);
            size_t index = static_cast<size_t>(tip->lParam);
            if (index < treePaths_.size()) wcsncpy_s(tip->pszText, tip->cchTextMax, treePaths_[index].c_str(), _TRUNCATE);
            break;
        }
        }
        return;
    }

    auto* scn = reinterpret_cast<SCNotification*>(nm);
    if (console_ && nm->hwndFrom == console_->hwnd()) {
        if (nm->code == SCN_DOUBLECLICK) ConsoleDoubleClick(scn->position);
        return;
    }
    Editor* ed = FindByHwnd(nm->hwndFrom);
    if (!ed) return;
    switch (nm->code) {
    case SCN_STYLENEEDED:
        ed->Lex(scn->position);
        break;
    case SCN_MODIFIED:
        if (scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
            ed->bumpVersion();
            if (ed == usageEditor_) ClearUsageHighlight();
            if (settings_->liveCheck) SetTimer(hwnd_, kCheckTimerBase + ed->id(), 700, nullptr);
        }
        break;
    case SCN_SAVEPOINTREACHED:
    case SCN_SAVEPOINTLEFT:
        InvalidateRect(hwnd_, &rcTabs_, FALSE);
        break;
    case SCN_UPDATEUI:
        InvalidateRect(hwnd_, &rcStatus_, FALSE);
        break;
    case SCN_CHARADDED: {
        ed->OnCharAdded(scn->ch);
        int ch = scn->ch;
        if (isalnum(ch) || ch == '_' || ch == '.' || ch == '%' || ch == '@' || ch == '?' || ch >= 0x80) Complete(ed, false);
        else if (ed->Call(SCI_AUTOCACTIVE)) ed->Call(SCI_AUTOCCANCEL);
        if (ch == ' ' || ch == '\t') ShowSignature(ed);
        else if (ch == '\n' || ch == '\r' || ch == ';') ed->Call(SCI_CALLTIPCANCEL);
        break;
    }
    case SCN_AUTOCCHARDELETED:  // Backspace while the list is open: filter again
        Complete(ed, false);
        break;
    case SCN_DWELLSTART:
        if (scn->position >= 0 && ed->DiagnosticsAt(static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, scn->position)) + 1)) {
            ed->ShowDiagnosticTip(scn->position);
        } else {
            ShowHint(ed, scn->position);
        }
        break;
    case SCN_MARGINCLICK:
        if (scn->margin == 0) ToggleBreakpoint(ed, static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, scn->position)) + 1);
        break;
    case SCN_DWELLEND:
        ed->Call(SCI_CALLTIPCANCEL);
        break;
    }
}

// ---- editors ----------------------------------------------------------------------------------------------------

Editor* MainWindow::FindByHwnd(HWND h) const {
    for (auto& e : editors_) if (e->hwnd() == h) return e.get();
    return nullptr;
}

Editor* MainWindow::FindById(int id) const {
    for (auto& e : editors_) if (e->id() == id) return e.get();
    return nullptr;
}

int MainWindow::FindByPath(const fs::path& p) const {
    for (size_t i = 0; i < editors_.size(); i++) if (SamePath(editors_[i]->file(), p)) return (int)i;
    return -1;
}

void MainWindow::AddEditor(std::unique_ptr<Editor> ed) {
    Editor* raw = ed.get();
    SetWindowSubclass(raw->hwnd(), EditorProc, 4, reinterpret_cast<DWORD_PTR>(this));
    editors_.push_back(std::move(ed));
    Layout();
    Activate((int)editors_.size() - 1);
    if (settings_->liveCheck) RequestCheck(raw);
}

void MainWindow::Activate(int index) {
    if (editors_.empty()) {
        active_ = -1;
    } else {
        active_ = std::clamp(index, 0, (int)editors_.size() - 1);
    }
    for (size_t i = 0; i < editors_.size(); i++) ShowWindow(editors_[i]->hwnd(), (int)i == active_ ? SW_SHOW : SW_HIDE);
    if (Editor* ed = Active()) SetFocus(ed->hwnd());
    ensureTabVisible_ = true;
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::ConfirmClose(int index) {
    Editor& ed = *editors_[index];
    if (!ed.Modified()) return true;
    Activate(index);
    std::wstring q = L"Сохранить изменения в «" + ed.Title() + L"»?";
    int answer = MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return false;
    return answer == IDNO || Save(&ed);
}

void MainWindow::CloseEditor(int index) {
    if (index < 0 || index >= (int)editors_.size() || !ConfirmClose(index)) return;
    StashBreakpoints(editors_[index].get());
    editors_.erase(editors_.begin() + index);
    if (active_ >= index) active_--;
    Activate(std::max(0, active_));
}

bool MainWindow::CloseAll() {
    for (int i = 0; i < (int)editors_.size(); i++) {
        if (!ConfirmClose(i)) return false;
    }
    for (auto& e : editors_) StashBreakpoints(e.get());
    editors_.clear();
    Activate(-1);
    return true;
}

Editor* MainWindow::OpenFile(const fs::path& path, int line) {
    fs::path p = NormalizePath(path);
    int existing = FindByPath(p);
    if (existing >= 0) {
        Activate(existing);
    } else {
        auto ed = std::make_unique<Editor>(hwnd_, settings_->fontSize);
        std::wstring error;
        if (!ed->Load(p, error)) {
            ShowMessage(hwnd_, L"Не удалось открыть " + p.wstring() + L": " + error);
            return nullptr;
        }
        auto bps = breakpoints_.find(Debugger::Key(p));
        if (bps != breakpoints_.end()) {
            ed->SetBreakpoints(bps->second);
            breakpoints_.erase(bps);  // the editor's markers are the source of truth while it is open
        }
        AddEditor(std::move(ed));
    }
    Editor* ed = Active();
    if (ed && line > 0) ed->GotoLine(line);
    return ed;
}

bool MainWindow::Save(Editor* ed) {
    if (!ed) return false;
    if (ed->file().empty()) return SaveAs(ed);
    std::wstring error;
    if (!ed->Save(ed->file(), error)) {
        ShowMessage(hwnd_, L"Не удалось сохранить: " + error);
        return false;
    }
    InvalidateChrome();
    return true;
}

bool MainWindow::SaveAs(Editor* ed) {
    if (!ed) return false;
    wchar_t buf[MAX_PATH * 4];
    wcsncpy_s(buf, ed->file().empty() ? ed->Title().c_str() : ed->file().c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Ассемблер (*.asm)\0*.asm\0Все файлы\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrDefExt = L"asm";
    std::wstring initial = projectRoot_.wstring();
    ofn.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    std::wstring error;
    if (!ed->Save(buf, error)) {
        ShowMessage(hwnd_, L"Не удалось сохранить: " + error);
        return false;
    }
    RefreshTree();
    UpdateTitle();
    InvalidateChrome();
    if (settings_->liveCheck) RequestCheck(ed);
    return true;
}

void MainWindow::SaveAll() {
    for (auto& e : editors_) {
        if (e->Modified()) Save(e.get());
    }
}

void MainWindow::ApplyFontSize(int size) {
    settings_->fontSize = std::clamp(size, 6, 40);
    for (auto& e : editors_) e->ApplyStyles(settings_->fontSize);
    console_->Call(SCI_STYLESETSIZE, STYLE_DEFAULT, settings_->fontSize);
    for (int s = 0; s <= static_cast<int>(Out::Input); s++) console_->Call(SCI_STYLESETSIZE, s, settings_->fontSize);
    settings_->Save();
}

// ---- project ----------------------------------------------------------------------------------------------------

std::optional<fs::path> MainWindow::PickFolder(const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    std::optional<fs::path> result;
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

void MainWindow::OpenPath(const fs::path& path) {
    if (DirExists(path)) OpenProject(path);
    else if (FileExists(path.wstring())) OpenFile(path);
    else ShowMessage(hwnd_, L"Не найдено: " + path.wstring());
}

bool MainWindow::OpenProject(const fs::path& dirIn) {
    fs::path dir = NormalizePath(dirIn);
    if (SamePath(dir, projectRoot_)) return true;
    SaveProjectSession();
    if (!CloseAll()) return false;
    projectRoot_ = dir;
    mainFile_.clear();
    AddRecent(dir);
    settings_->lastProject = dir.wstring();
    settings_->Save();

    auto kv = ReadKeyValues(dir / L".asmeditor" / L"project.ini");
    if (!kv[L"main"].empty() && FileExists((dir / kv[L"main"]).wstring())) mainFile_ = NormalizePath(dir / kv[L"main"]);
    excludedFromBuild_.clear();
    for (const std::wstring& rel : Split(kv[L"exclude"], L'|')) excludedFromBuild_.insert(Project::Key(dir / rel));
    for (const std::wstring& entry : Split(kv[L"breakpoints"], L'|')) {  // "relative*3,7,12"
        size_t star = entry.find(L'*');
        if (star == std::wstring::npos) continue;
        auto& lines = breakpoints_[Debugger::Key(dir / entry.substr(0, star))];
        for (const std::wstring& n : Split(entry.substr(star + 1), L',')) lines.insert(_wtoi(n.c_str()));
    }
    for (const std::wstring& f : Split(kv[L"open"], L'|')) {
        fs::path p = dir / f;
        if (FileExists(p.wstring())) OpenFile(p);
    }
    int active = kv[L"active"].empty() ? -1 : _wtoi(kv[L"active"].c_str());
    if (active >= 0 && active < (int)editors_.size()) Activate(active);
    RefreshTree();
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void MainWindow::CloseProject() {
    if (projectRoot_.empty()) return;
    SaveProjectSession();
    if (!CloseAll()) return;
    projectRoot_.clear();
    mainFile_.clear();
    settings_->lastProject.clear();
    settings_->Save();
    RefreshTree();
    UpdateTitle();
}

void MainWindow::SaveProjectSession() {
    if (projectRoot_.empty()) {
        settings_->sessionFiles.clear();
        for (auto& e : editors_) {
            if (!e->file().empty()) settings_->sessionFiles.push_back(e->file().wstring());
        }
        return;
    }
    std::vector<std::wstring> open;
    for (auto& e : editors_) {
        if (IsUnder(e->file(), projectRoot_)) open.push_back(e->file().lexically_relative(projectRoot_).wstring());
    }
    std::vector<std::wstring> bps;
    for (const auto& [key, lines] : CollectBreakpoints()) {
        if (!IsUnder(key, projectRoot_) || lines.empty()) continue;
        std::vector<std::wstring> nums;
        for (int l : lines) nums.push_back(std::to_wstring(l));
        bps.push_back(fs::path(key).lexically_relative(Lower(NormalizePath(projectRoot_).wstring())).wstring() + L"*" +
                      Join(nums, L","));
    }
    std::vector<std::wstring> excluded;
    for (const std::wstring& key : excludedFromBuild_) {
        if (IsUnder(key, projectRoot_)) {
            excluded.push_back(fs::path(key).lexically_relative(Lower(NormalizePath(projectRoot_).wstring())).wstring());
        }
    }
    std::wstring text = L"main=" + (IsUnder(mainFile_, projectRoot_) ? mainFile_.lexically_relative(projectRoot_).wstring() : L"") +
                        L"\r\nopen=" + Join(open, L"|") + L"\r\nactive=" + std::to_wstring(active_) +
                        L"\r\nbreakpoints=" + Join(bps, L"|") + L"\r\nexclude=" + Join(excluded, L"|") + L"\r\n";
    fs::path dir = projectRoot_ / L".asmeditor";
    std::error_code ec;
    fs::create_directories(dir, ec);
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    std::ofstream out(dir / L"project.ini", std::ios::binary | std::ios::trunc);
    std::string bytes = Narrow(text);
    out.write(bytes.data(), (std::streamsize)bytes.size());
}

void MainWindow::RestoreSession() {
    if (!settings_->lastProject.empty() && DirExists(settings_->lastProject)) {
        OpenProject(settings_->lastProject);
        return;
    }
    for (const std::wstring& f : settings_->sessionFiles) {
        if (FileExists(f)) OpenFile(f);
    }
}

void MainWindow::NewProject() {
    auto dir = PickFolder(L"Папка для нового проекта (можно создать новую)");
    if (!dir) return;
    fs::path main = *dir / L"main.asm";
    if (!fs::exists(main)) {
        std::ofstream out(main, std::ios::binary);
        out << kTemplate;
    }
    if (!OpenProject(*dir)) return;
    SetMainFile(main);
    OpenFile(main);
    RefreshTree();
}

void MainWindow::SetMainFile(const fs::path& f) {
    mainFile_ = f.empty() ? fs::path() : NormalizePath(f);
    SaveProjectSession();
    RefreshTree();
    InvalidateChrome();
}

void MainWindow::AddRecent(const fs::path& dir) {
    auto& r = settings_->recent;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& s) { return SamePath(s, dir); }), r.end());
    r.insert(r.begin(), dir.wstring());
    if (r.size() > 10) r.resize(10);
}

fs::path MainWindow::TreePath(HTREEITEM item) const {
    if (!item) return {};
    TVITEMW tvi{};
    tvi.mask = TVIF_PARAM;
    tvi.hItem = item;
    TreeView_GetItem(tree_, &tvi);
    size_t index = static_cast<size_t>(tvi.lParam);
    return index < treePaths_.size() ? treePaths_[index] : fs::path();
}

void MainWindow::BuildTree(HTREEITEM parent, const fs::path& dir, int depth) {
    std::vector<fs::path> dirs, files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (!kHiddenDirs.count(it->path().filename().wstring())) dirs.push_back(it->path());
        } else {
            files.push_back(it->path());
        }
    }
    auto byName = [](const fs::path& a, const fs::path& b) {
        return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    auto insert = [&](const fs::path& p, bool folder) {
        SHFILEINFOW sfi{};
        SHGetFileInfoW(p.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        std::wstring name = p.filename().wstring();
        bool isMain = SamePath(p, mainFile_);
        if (isMain) name += L"   ▶ main";
        else if (!folder && excludedFromBuild_.count(Project::Key(p))) name += L"   — не собирается";
        TVINSERTSTRUCTW ins{};
        ins.hParent = parent;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_STATE | TVIF_CHILDREN;
        ins.item.pszText = name.data();
        ins.item.iImage = ins.item.iSelectedImage = sfi.iIcon;
        ins.item.lParam = static_cast<LPARAM>(treePaths_.size());
        ins.item.state = isMain ? TVIS_BOLD : 0;
        ins.item.stateMask = TVIS_BOLD;
        ins.item.cChildren = folder ? 1 : 0;
        treePaths_.push_back(p);
        HTREEITEM item = TreeView_InsertItem(tree_, &ins);
        if (folder && depth < 16) BuildTree(item, p, depth + 1);
    };
    for (const fs::path& d : dirs) insert(d, true);
    for (const fs::path& f : files) insert(f, false);
}

void MainWindow::RefreshTree() {
    // Remember expanded folders and the selection.
    std::set<std::wstring> expanded;
    std::function<void(HTREEITEM)> collect = [&](HTREEITEM item) {
        for (; item; item = TreeView_GetNextSibling(tree_, item)) {
            if (TreeView_GetItemState(tree_, item, TVIS_EXPANDED) & TVIS_EXPANDED) {
                expanded.insert(Lower(TreePath(item).wstring()));
                collect(TreeView_GetChild(tree_, item));
            }
        }
    };
    collect(TreeView_GetRoot(tree_));
    std::wstring selected = Lower(TreePath(TreeView_GetSelection(tree_)).wstring());

    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    treePaths_.clear();
    if (!projectRoot_.empty()) {
        SHFILEINFOW sfi{};
        SHGetFileInfoW(projectRoot_.c_str(), FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        std::wstring name = projectRoot_.filename().wstring();
        if (name.empty()) name = projectRoot_.wstring();
        TVINSERTSTRUCTW ins{};
        ins.hParent = TVI_ROOT;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_STATE;
        ins.item.pszText = name.data();
        ins.item.iImage = ins.item.iSelectedImage = sfi.iIcon;
        ins.item.lParam = 0;
        ins.item.state = TVIS_BOLD;
        ins.item.stateMask = TVIS_BOLD;
        treePaths_.push_back(projectRoot_);
        HTREEITEM root = TreeView_InsertItem(tree_, &ins);
        BuildTree(root, projectRoot_, 0);
        expanded.insert(Lower(projectRoot_.wstring()));

        std::function<void(HTREEITEM)> restore = [&](HTREEITEM item) {
            for (; item; item = TreeView_GetNextSibling(tree_, item)) {
                std::wstring key = Lower(TreePath(item).wstring());
                if (expanded.count(key)) {
                    TreeView_Expand(tree_, item, TVE_EXPAND);
                    restore(TreeView_GetChild(tree_, item));
                }
                if (key == selected) TreeView_SelectItem(tree_, item);
            }
        };
        restore(root);
    }
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, TRUE);
    InvalidateRect(hwnd_, &rcTreeHeader_, FALSE);
}

void MainWindow::TreeContextMenu() {
    if (projectRoot_.empty()) return;
    fs::path target = contextItem_ ? TreePath(contextItem_) : projectRoot_;
    bool file = !target.empty() && !DirExists(target);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TREE_NEW_FILE, L"Новый файл…");
    AppendMenuW(menu, MF_STRING, ID_TREE_NEW_FOLDER, L"Новая папка…");
    if (file) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TREE_OPEN, L"Открыть");
        AppendMenuW(menu, MF_STRING, ID_TREE_MAIN,
                    SamePath(target, mainFile_) ? L"Убрать отметку главного файла" : L"Сделать главным файлом (запуск)");
        if (Project::IsSourceFile(target)) {
            AppendMenuW(menu, MF_STRING, ID_TREE_EXCLUDE, excludedFromBuild_.count(Project::Key(target))
                                                               ? L"Собирать этот файл"
                                                               : L"Не собирать этот файл");
        }
    }
    if (!SamePath(target, projectRoot_)) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TREE_RENAME, L"Переименовать…");
        AppendMenuW(menu, MF_STRING, ID_TREE_DELETE, L"Удалить в корзину");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TREE_REVEAL, L"Показать в проводнике");
    AppendMenuW(menu, MF_STRING, ID_REFRESH_TREE, L"Обновить");
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
}

// ---- build & run ------------------------------------------------------------------------------------------------

std::vector<fs::path> MainWindow::IncludeDirs(const fs::path& file) const {
    std::vector<fs::path> dirs;
    if (!file.empty()) dirs.push_back(file.parent_path());
    if (!projectRoot_.empty() && (dirs.empty() || !SamePath(dirs[0], projectRoot_))) dirs.push_back(projectRoot_);
    return dirs;
}

fs::path MainWindow::BuildTarget() {
    if (!mainFile_.empty() && FileExists(mainFile_.wstring())) return mainFile_;
    Editor* ed = Active();
    if (!ed) {
        ShowMessage(hwnd_, L"Откройте .asm файл или выберите главный файл проекта "
                           L"(ПКМ по файлу в дереве → «Сделать главным файлом»).", false);
        return {};
    }
    if (ed->file().empty() && !SaveAs(ed)) return {};
    return ed->file();
}

void MainWindow::StartBuild(After after, bool rebuild) {
    if (building_ || debugging_) return;
    if (!Toolchain::NasmAvailable(*settings_)) {
        ShowMessage(hwnd_, L"NASM не найден. Укажите путь к nasm.exe в настройках.");
        OnCommand(ID_SETTINGS);
        return;
    }
    fs::path src = BuildTarget();
    if (src.empty()) return;
    StopProgram();
    SaveAll();
    fs::path outDir = (IsUnder(src, projectRoot_) ? projectRoot_ : src.parent_path()) / L"build";
    OnCommand(ID_CLEAR_CONSOLE);
    building_ = true;
    InvalidateChrome();
    Settings copy = *settings_;
    BuildPlan plan;
    plan.outDir = outDir;
    plan.rebuild = rebuild;
    plan.exe = outDir / (src.stem().wstring() + L".exe");
    if (IsUnder(src, projectRoot_)) {  // a project: link all its source files into one program
        Project::Sources sources = Project::Collect(projectRoot_, src, excludedFromBuild_);
        plan.sources = sources.files;
        plan.root = projectRoot_;
        plan.includes = {projectRoot_};
        for (const fs::path& other : sources.otherPrograms) {
            Print("Пропущен (своя функция main): " + Narrow(other.filename().wstring()) + "\n", Out::Info);
        }
    } else {
        plan.sources = {src};
        plan.root = src.parent_path();
    }
    HWND hwnd = hwnd_;
    int afterCode = static_cast<int>(after);
    std::thread([copy, plan, src, hwnd, afterCode] {
        auto sink = [hwnd](const std::string& text, Out style) {
            PostMessageW(hwnd, WM_APP_OUTPUT, static_cast<WPARAM>(style), reinterpret_cast<LPARAM>(new std::string(text)));
        };
        auto* done = new BuildDone{Toolchain::Build(copy, plan, sink), afterCode, src};
        PostMessageW(hwnd, WM_APP_BUILD_DONE, 0, reinterpret_cast<LPARAM>(done));
    }).detach();
}

void MainWindow::OnBuildDone(BuildResult* r, After after, const fs::path& src) {
    building_ = false;
    for (auto& e : editors_) {
        if (e->file().empty()) continue;
        std::vector<Diagnostic> mine;
        for (const Diagnostic& d : r->diagnostics) {
            if (SamePath(d.file, e->file())) mine.push_back(d);
        }
        if (!mine.empty() || SamePath(e->file(), src)) e->SetDiagnostics(mine);
    }
    for (const Diagnostic& d : r->diagnostics) {
        if (d.severity == Severity::Error && FileExists(d.file.wstring())) {
            OpenFile(d.file, d.line);
            break;
        }
    }
    RefreshTree();
    InvalidateChrome();
    if (r->ok && after != After::Nothing) RunProgram(r->exe, IsUnder(src, projectRoot_) ? projectRoot_ : src.parent_path(), after);
}

UINT MainWindow::ProgramCodePage() const {
    if (settings_->programEncoding == L"cp866") return 866;
    if (settings_->programEncoding == L"windows-1251") return 1251;
    return CP_UTF8;
}

void MainWindow::RunProgram(const fs::path& exe, const fs::path& cwd, After mode) {
    bool debug = mode == After::Debug || mode == After::DebugEntry;
    if (settings_->externalConsole && !debug) {
        RunExternal(exe, cwd);
        return;
    }
    Print(std::string("\n") + (debug ? "Отладка " : "Запуск ") + Narrow(exe.filename().wstring()) + "\n", Out::Info);
    if (!StartConsole()) {
        Print("Не удалось создать консоль для программы.\n", Out::Error);
        return;
    }
    UINT codePage = ProgramCodePage();
    Toolchain::SetConsoleCodePage(pty_, codePage);
    std::wstring cmd = Toolchain::QuoteCommandLine({exe.wstring()});

    if (debug) {
        if (!fs::exists(fs::path(exe).replace_extension(L".pdb"))) {
            Print("Нет .pdb рядом с exe — отладка будет без исходников. Для отладки нужны шаблон линковки MSVC "
                  "с /debug и аргументы NASM «-g -F cv8».\n", Out::Warning);
        }
        debugger_ = std::make_unique<Debugger>(hwnd_);
        std::wstring error;
        if (!debugger_->Start(exe, cwd, pty_, CollectBreakpoints(), mode == After::DebugEntry, error)) {
            debugger_.reset();
            Print("Не удалось запустить отладку: " + Narrow(error) + "\n", Out::Error);
            FinishConsole(static_cast<DWORD>(-1));
            return;
        }
        SetDebugging(true);
        SetFocus(console_->hwnd());
        return;
    }

    PROCESS_INFORMATION pi{};
    if (!Toolchain::SpawnInConsole(cmd, cwd, pty_, CREATE_SUSPENDED, pi)) {
        Print("Не удалось запустить программу (код " + std::to_string(GetLastError()) + ")\n", Out::Error);
        FinishConsole(static_cast<DWORD>(-1));
        return;
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    DWORD generation = ++generation_;
    SetFocus(console_->hwnd());
    InvalidateChrome();

    HWND hwnd = hwnd_;
    HANDLE process = process_;
    std::thread([hwnd, process, generation] {
        WaitForSingleObject(process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        PostMessageW(hwnd, WM_APP_PROC_EXIT, code, generation);
    }).detach();
}

void MainWindow::OnProgramExit(DWORD code, DWORD generation) {
    if (generation != generation_ || !process_) return;
    CloseHandle(process_);
    process_ = nullptr;
    if (job_) CloseHandle(job_);
    job_ = nullptr;
    FinishConsole(code);  // reports the exit code once the remaining output is shown
    InvalidateChrome();
}

void MainWindow::StopProgram() {
    if (debugging_ && debugger_) {
        debugger_->Stop();
        Print("\n[отладка остановлена]\n", Out::Warning);
        return;
    }
    if (!process_) return;
    if (job_) TerminateJobObject(job_, 1);
    else TerminateProcess(process_, 1);
    Print("\n[остановлено пользователем]\n", Out::Warning);
}

// ---- debugging --------------------------------------------------------------------------------------------------

void MainWindow::DebugCommand(int id) {
    if (!debugging_) {
        if (id == ID_DEBUG) StartBuild(After::Debug);
        else if (id == ID_STEP_OVER || id == ID_STEP_INTO) StartBuild(After::DebugEntry);
        return;
    }
    if (id == ID_PAUSE) {
        debugger_->Pause();
        return;
    }
    if (!debugger_->Stopped()) return;
    ClearExecutionLine();
    debugStatus_ = L"выполняется…";
    debugStatusColor_ = Theme::Success;
    switch (id) {
    case ID_DEBUG: debugger_->Continue(); break;
    case ID_STEP_OVER: debugger_->StepOver(); break;
    case ID_STEP_INTO: debugger_->StepInto(); break;
    case ID_STEP_OUT: debugger_->StepOut(); break;
    }
    // Stopped() turns false on the debugger thread; repaint once it has picked up the command.
    InvalidateChrome();
    SetTimer(hwnd_, 1, 50, [](HWND h, UINT, UINT_PTR id, DWORD) {
        KillTimer(h, id);
        InvalidateRect(h, nullptr, FALSE);
    });
    // If the program keeps running (e.g. waits in scanf), typing should go to it, not into the source.
    SetTimer(hwnd_, kConsoleFocusTimer, 400, nullptr);
}

void MainWindow::SetDebugging(bool on) {
    debugging_ = on;
    haveLastContext_ = false;
    if (on) {
        debugStatus_ = L"выполняется…";
        debugStatusColor_ = Theme::Success;
        RenderPanel(*regsView_, {{"Программа выполняется.\nПоставьте точку останова (F9 или клик слева от номера "
                                  "строки) или нажмите «Пауза».\n", P_DIM}});
        RenderPanel(*memView_, {});
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnDebugStopped(std::unique_ptr<DebugStop> stop) {
    if (!debugging_) return;
    switch (stop->reason) {
    case DebugStop::Reason::Breakpoint: debugStatus_ = L"точка останова"; break;
    case DebugStop::Reason::Step: debugStatus_ = L"шаг"; break;
    case DebugStop::Reason::Entry: debugStatus_ = L"начало main"; break;
    case DebugStop::Reason::Pause: debugStatus_ = L"пауза"; break;
    case DebugStop::Reason::Int3: debugStatus_ = L"int3 в программе"; break;
    case DebugStop::Reason::Exception: debugStatus_ = L"исключение"; break;
    }
    debugStatusColor_ = stop->reason == DebugStop::Reason::Exception ? Theme::Error : Theme::Warning;

    std::wstring where = stop->location.valid()
                             ? stop->location.file.filename().wstring() + L":" + std::to_wstring(stop->location.line)
                             : debugger_->SymbolAt(stop->context.Rip);
    if (where.empty()) {
        wchar_t buf[32];
        swprintf_s(buf, L"0x%llX", static_cast<unsigned long long>(stop->context.Rip));
        where = buf;
    }
    if (stop->reason == DebugStop::Reason::Exception) {
        Print(Narrow(stop->message + L"\n    в " + where) + "\n", Out::Error);
    } else if (!stop->location.valid()) {
        Print("Остановлено вне исходного кода: " + Narrow(where) + "\n", Out::Info);
    }

    ClearExecutionLine();
    if (stop->location.valid()) {
        if (Editor* ed = OpenFile(stop->location.file, stop->location.line)) {
            ed->SetExecutionLine(stop->location.line);
            execFile_ = stop->location.file;
        }
    }
    RenderRegisters();
    RenderMemory();
    lastContext_ = stop->context;
    haveLastContext_ = true;
    if (GetForegroundWindow() != hwnd_) SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnDebugExited(DWORD code) {
    if (!debugging_) return;
    debugger_.reset();
    ClearExecutionLine();
    SetDebugging(false);
    FinishConsole(code);
}

void MainWindow::ClearExecutionLine() {
    for (auto& e : editors_) e->SetExecutionLine(0);
    execFile_.clear();
}

void MainWindow::ToggleBreakpoint(Editor* ed, int line) {
    if (!ed || line <= 0) return;
    ed->ToggleBreakpoint(line);
    if (debugger_ && !ed->file().empty()) debugger_->SetBreakpoints(ed->file(), ed->Breakpoints());
}

void MainWindow::StashBreakpoints(Editor* ed) {
    if (!ed || ed->file().empty()) return;
    std::set<int> lines = ed->Breakpoints();
    if (lines.empty()) breakpoints_.erase(Debugger::Key(ed->file()));
    else breakpoints_[Debugger::Key(ed->file())] = lines;
}

Debugger::Breakpoints MainWindow::CollectBreakpoints() const {
    Debugger::Breakpoints all = breakpoints_;
    for (const auto& e : editors_) {
        if (e->file().empty()) continue;
        std::set<int> lines = e->Breakpoints();
        if (lines.empty()) all.erase(Debugger::Key(e->file()));
        else all[Debugger::Key(e->file())] = lines;
    }
    return all;
}

void MainWindow::RenderPanel(Editor& v, const Runs& runs) {
    Sci_Position first = v.Call(SCI_GETFIRSTVISIBLELINE);
    v.Call(SCI_SETREADONLY, 0);
    v.Call(SCI_CLEARALL);
    for (const auto& [text, style] : runs) {
        Sci_Position pos = v.Call(SCI_GETLENGTH);
        v.Call(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.data()));
        v.Call(SCI_STARTSTYLING, pos);
        v.Call(SCI_SETSTYLING, text.size(), style);
    }
    v.Call(SCI_SETREADONLY, 1);
    v.Call(SCI_SETFIRSTVISIBLELINE, first);
}

void MainWindow::RenderRegisters() {
    if (!DebugStopped()) return;
    const CONTEXT& c = debugger_->Context();
    const CONTEXT* prev = haveLastContext_ ? &lastContext_ : nullptr;
    Runs r;
    auto annotate = [&](uint64_t v) {
        std::wstring sym = debugger_->SymbolAt(v);
        if (!sym.empty()) r.push_back({"  " + Narrow(sym), P_SYMBOL});
        else if (v <= 0xFFFFF) r.push_back({"  " + std::to_string(v), P_DIM});
        else if (static_cast<int64_t>(v) < 0 && static_cast<int64_t>(v) > -0x100000)
            r.push_back({"  " + std::to_string(static_cast<int64_t>(v)), P_DIM});
    };

    r.push_back({"РЕГИСТРЫ\n", P_HEADER});
    static const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rip"};
    for (const char* n : names) {
        uint64_t v = *Debugger::Register(c, n);
        bool changed = prev && *Debugger::Register(*prev, n) != v;
        r.push_back({PadRight(n, 4), P_LABEL});
        r.push_back({Hex16(v), changed ? P_CHANGED : P_TEXT});
        annotate(v);
        r.push_back({"\n", P_TEXT});
    }
    r.push_back({"flg ", P_LABEL});
    static const std::pair<const char*, int> flags[] = {{"CF", 0}, {"PF", 2}, {"AF", 4}, {"ZF", 6}, {"SF", 7},
                                                        {"TF", 8}, {"IF", 9}, {"DF", 10}, {"OF", 11}};
    for (auto [name, bit] : flags) {
        bool on = (c.EFlags >> bit) & 1;
        bool changed = prev && (((prev->EFlags >> bit) & 1) != on);
        r.push_back({std::string(name) + (on ? "=1 " : "=0 "), changed ? P_CHANGED : on ? P_FLAG_ON : P_DIM});
    }
    r.push_back({"\n\nСТЕК\n", P_HEADER});
    uint64_t stack[16]{};
    size_t got = debugger_->ReadMemory(c.Rsp, stack, sizeof(stack)) / 8;
    for (size_t i = 0; i < got; i++) {
        char label[16];
        sprintf_s(label, "rsp+%02zX ", i * 8);
        r.push_back({label, P_ADDR});
        r.push_back({Hex16(stack[i]), P_TEXT});
        annotate(stack[i]);
        if (c.Rsp + i * 8 == c.Rbp) r.push_back({"  ← rbp", P_DIM});
        r.push_back({"\n", P_TEXT});
    }
    r.push_back({"\nXMM (старшие · младшие 64 бита · младший double)\n", P_HEADER});
    for (int i = 0; i < 16; i++) {
        const M128A& x = c.FltSave.XmmRegisters[i];
        bool changed = prev && (prev->FltSave.XmmRegisters[i].Low != x.Low || prev->FltSave.XmmRegisters[i].High != x.High);
        r.push_back({PadRight("xmm" + std::to_string(i), 6), P_LABEL});
        r.push_back({Hex16(static_cast<uint64_t>(x.High)) + " " + Hex16(x.Low), changed ? P_CHANGED : P_TEXT});
        double d;
        memcpy(&d, &x.Low, 8);
        char buf[48];
        sprintf_s(buf, "  %g\n", d);
        r.push_back({buf, P_DIM});
    }
    RenderPanel(*regsView_, r);
}

void MainWindow::RenderMemory() {
    if (!DebugStopped()) return;
    int n = GetWindowTextLengthW(memExpr_);
    std::wstring w(n, L'\0');
    GetWindowTextW(memExpr_, w.data(), n + 1);
    std::string expr = Narrow(Trim(w));
    if (expr.empty()) expr = "rsp";
    Runs r;
    std::wstring error;
    auto addr = debugger_->Evaluate(expr, error);
    if (!addr) {
        RenderPanel(*memView_, {{Narrow(error) + "\n", P_ERROR}});
        return;
    }
    r.push_back({expr + " = 0x" + Hex16(*addr), P_TEXT});
    std::wstring sym = debugger_->SymbolAt(*addr);
    if (!sym.empty()) r.push_back({"  " + Narrow(sym), P_SYMBOL});
    r.push_back({"\n", P_TEXT});
    uint8_t bytes[256];
    size_t got = debugger_->ReadMemory(*addr, bytes, sizeof(bytes));
    if (got == 0) r.push_back({"память недоступна\n", P_ERROR});
    for (size_t row = 0; row < got; row += 8) {
        r.push_back({Hex16(*addr + row) + "  ", P_ADDR});
        std::string hex, ascii;
        for (size_t i = 0; i < 8; i++) {
            if (row + i < got) {
                char b[4];
                sprintf_s(b, "%02X ", bytes[row + i]);
                hex += b;
                unsigned char ch = bytes[row + i];
                ascii += (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '.';
            } else {
                hex += "   ";
            }
        }
        r.push_back({hex, P_TEXT});
        r.push_back({" " + ascii + "\n", P_DIM});
    }
    RenderPanel(*memView_, r);
}

std::string MainWindow::DebugTipText(Editor* ed, Sci_Position pos) {
    if (pos < 0 || !DebugStopped()) return {};
    std::string word = ed->WordAt(pos);
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0]))) return {};
    std::string lower = word;
    for (char& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    std::string tip;
    if (auto reg = Debugger::Register(debugger_->Context(), lower)) {
        char buf[64];
        sprintf_s(buf, " = 0x%llX  (%lld)", static_cast<unsigned long long>(*reg), static_cast<long long>(*reg));
        tip = word + buf;
    } else if (auto addr = debugger_->LookupSymbol(word)) {
        uint8_t bytes[16];
        size_t got = debugger_->ReadMemory(*addr, bytes, sizeof(bytes));
        tip = word + " @ 0x" + Hex16(*addr);
        if (got >= 8) {
            uint64_t q;
            memcpy(&q, bytes, 8);
            tip += "\nqword: 0x" + Hex16(q) + "  (" + std::to_string(static_cast<int64_t>(q)) + ")";
        }
        if (got) {
            std::string hex, ascii;
            for (size_t i = 0; i < got; i++) {
                char b[4];
                sprintf_s(b, "%02X ", bytes[i]);
                hex += b;
                ascii += (bytes[i] >= 32 && bytes[i] < 127) ? static_cast<char>(bytes[i]) : '.';
            }
            tip += "\nбайты: " + hex + " " + ascii;
        }
    }
    return tip;
}

// Reference card for the instruction / register / directive under the mouse or caret (with the register's value
// while the debugger is stopped).
void MainWindow::ShowHint(Editor* ed, Sci_Position pos) {
    if (pos < 0) return;
    Sci_Position start, end;
    if (!ed->WordRange(pos, start, end)) return;
    std::string word = ed->WordAt(pos);
    if (start > 0 && ed->Call(SCI_GETCHARAT, start - 1) == '%') {  // %define, %macro…
        word = "%" + word;
        start--;
    }
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, start));
    std::string card;
    if (style == T_INSTRUCTION || style == T_REGISTER || style == T_DIRECTIVE || style == T_PREPROCESSOR ||
        style == T_KEYWORD) {
        card = Hints::For(word);
    }
    std::string debug = DebugTipText(ed, pos);
    if (card.empty() && debug.empty()) return;
    std::string tip = debug.empty() ? card : card.empty() ? debug : debug + "\n\n" + card;
    ed->Call(SCI_CALLTIPSHOW, start, reinterpret_cast<sptr_t>(tip.c_str()));
    if (!card.empty()) {
        size_t from = debug.empty() ? 0 : debug.size() + 2;
        ed->Call(SCI_CALLTIPSETHLT, from, from + Hints::HeaderLength(card));
    }
}

// After "mnemonic " is typed: a one-line reminder of the operands.
void MainWindow::ShowSignature(Editor* ed) {
    Sci_Position pos = ed->Call(SCI_GETCURRENTPOS);
    Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, ed->Call(SCI_LINEFROMPOSITION, pos));
    std::string before(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, lineStart, pos - lineStart)),
                       pos - lineStart);
    size_t colon = before.find(':');
    size_t from = colon != std::string::npos && before.find_first_of(";\"'") > colon ? colon + 1 : 0;
    size_t s = before.find_first_not_of(" \t", from);
    if (s == std::string::npos) return;
    size_t e = before.find_first_of(" \t", s);
    if (e == std::string::npos || before.find_first_not_of(" \t", e) != std::string::npos) return;  // operands begun
    std::string mnemonic = before.substr(s, e - s);
    std::string sig = Hints::Signature(mnemonic);
    if (sig.empty()) return;
    ed->Call(SCI_CALLTIPSHOW, lineStart + s, reinterpret_cast<sptr_t>(sig.c_str()));
    ed->Call(SCI_CALLTIPSETHLT, 0, Hints::HeaderLength(sig));
}

void MainWindow::RunExternal(const fs::path& exe, const fs::path& cwd) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path bat = fs::path(tmp) / L"asmeditor-run.bat";
    std::wstring script = L"@echo off\r\n";
    if (ProgramCodePage() == CP_UTF8) script += L"chcp 65001 >nul\r\n";
    script += L"title " + exe.filename().wstring() + L"\r\n";
    script += L"cd /d \"" + cwd.wstring() + L"\"\r\n";
    script += L"\"" + exe.wstring() + L"\"\r\n";
    script += L"echo.\r\necho Process exited with code %errorlevel%\r\npause >nul\r\n";
    {
        std::ofstream out(bat, std::ios::binary | std::ios::trunc);
        std::string bytes = Narrow(script, CP_OEMCP);
        out.write(bytes.data(), (std::streamsize)bytes.size());
    }
    std::wstring cmd = L"cmd.exe /c \"" + bat.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Print("\nПрограмма запущена во внешнем окне консоли.\n", Out::Info);
    } else {
        Print("Не удалось открыть консоль.\n", Out::Error);
    }
}

// ---- program console (ConPTY) -----------------------------------------------------------------------------------

std::pair<int, int> MainWindow::ConsoleGridSize() const {
    Editor& c = *console_;
    RECT rc;
    GetClientRect(c.hwnd(), &rc);
    int charW = std::max(1, static_cast<int>(c.Call(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>("M"))));
    int lineH = std::max(1, static_cast<int>(c.Call(SCI_TEXTHEIGHT, 0)));
    int cols = (rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL) - 12) / charW;
    int rows = (rc.bottom - rc.top) / lineH;
    return {std::clamp(cols, 20, 500), std::clamp(rows, 4, 200)};
}

bool MainWindow::StartConsole() {
    EndConsole();
    if (pty_ || ptyReader_.joinable()) FinishConsole(static_cast<DWORD>(-1));  // previous program still closing
    auto [cols, rows] = ConsoleGridSize();
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0)) return false;
    if (!CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return false;
    }
    HPCON pty = nullptr;
    HRESULT hr = CreatePseudoConsole(COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)}, inRead, outWrite, 0, &pty);
    CloseHandle(inRead);  // the pseudoconsole keeps its own duplicates
    CloseHandle(outWrite);
    if (FAILED(hr)) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }
    pty_ = pty;
    ptyInput_ = inWrite;
    term_ = Terminal(cols, rows);
    termStart_ = console_->Call(SCI_GETLENGTH);
    termActive_ = true;
    pendingSurrogate_ = 0;
    DWORD session = ++consoleSession_;
    HWND hwnd = hwnd_;
    ptyReader_ = std::thread([hwnd, outRead, session] {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            PostMessageW(hwnd, WM_APP_PTY_OUTPUT, session, reinterpret_cast<LPARAM>(new std::string(buf, n)));
        }
        CloseHandle(outRead);
    });
    console_->Call(SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
    return true;
}

// Stops mirroring the terminal in the view (the text stays) and stops sending keys to the program.
void MainWindow::EndConsole() {
    if (!termActive_) return;
    RenderTerminal();
    termActive_ = false;
    if (ptyInput_) CloseHandle(ptyInput_);
    ptyInput_ = nullptr;
    console_->Call(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
}

// After the program exits: close the pseudoconsole (this flushes its last output), wait for the reader to drain
// it, then report the exit code. Runs in the background because ClosePseudoConsole can block.
void MainWindow::FinishConsole(DWORD exitCode) {
    HPCON pty = pty_;
    pty_ = nullptr;
    std::thread reader = std::move(ptyReader_);
    HWND hwnd = hwnd_;
    DWORD session = consoleSession_;
    std::thread([pty, reader = std::move(reader), hwnd, session, exitCode]() mutable {
        if (pty) ClosePseudoConsole(pty);
        if (reader.joinable()) reader.join();
        PostMessageW(hwnd, WM_APP_PTY_DONE, exitCode, session);
    }).detach();
}

void MainWindow::OnConsoleDone(DWORD exitCode) {
    EndConsole();
    if (exitCode != static_cast<DWORD>(-1)) {
        Print("\nПроцесс завершён с кодом " + Narrow(Toolchain::DescribeExitCode(exitCode)) + "\n",
              exitCode == 0 ? Out::Info : Out::Error);
    }
    InvalidateChrome();
}

void MainWindow::RenderTerminal() {
    Editor& c = *console_;
    std::vector<TermLine> history = term_.TakeHistory();
    size_t cursor = 0;
    std::vector<TermLine> screen = term_.Screen(cursor);
    c.Call(SCI_SETREADONLY, 0);
    Sci_Position len = c.Call(SCI_GETLENGTH);
    termStart_ = std::min(termStart_, len);
    c.Call(SCI_DELETERANGE, termStart_, len - termStart_);
    auto append = [&](const TermLine& line, bool newline) {
        Sci_Position pos = c.Call(SCI_GETLENGTH);
        std::string text = line.text + (newline ? "\n" : "");
        c.Call(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.data()));
        c.Call(SCI_STARTSTYLING, pos);
        for (auto [bytes, style] : line.runs) c.Call(SCI_SETSTYLING, bytes, style);
        if (newline) c.Call(SCI_SETSTYLING, 1, 0);
    };
    for (const TermLine& line : history) append(line, true);
    termStart_ = c.Call(SCI_GETLENGTH);
    for (size_t i = 0; i < screen.size(); i++) append(screen[i], i + 1 < screen.size());
    c.Call(SCI_SETREADONLY, 1);
    TrimConsole();
    if (termActive_) {
        c.Call(SCI_GOTOPOS, std::min<Sci_Position>(termStart_ + cursor, c.Call(SCI_GETLENGTH)));
        c.Call(SCI_SCROLLCARET);
    }
}

void MainWindow::TrimConsole() {
    Editor& c = *console_;
    Sci_Position len = c.Call(SCI_GETLENGTH);
    if (len <= 4'000'000) return;
    Sci_Position cut = c.Call(SCI_POSITIONFROMLINE, c.Call(SCI_LINEFROMPOSITION, len / 2));
    if (termActive_) cut = std::min(cut, termStart_);
    c.Call(SCI_SETREADONLY, 0);
    c.Call(SCI_DELETERANGE, 0, cut);
    c.Call(SCI_SETREADONLY, 1);
    termStart_ = std::max<Sci_Position>(0, termStart_ - cut);
}

void MainWindow::SendToProgram(const std::string& bytes) {
    if (!ptyInput_ || bytes.empty()) return;
    DWORD written = 0;
    WriteFile(ptyInput_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

// Keyboard input typed into the console view goes to the program as terminal (VT) input.
bool MainWindow::ConsoleKey(UINT msg, WPARAM wp) {
    if (!termActive_ || !ptyInput_) return false;
    Editor& c = *console_;
    if (msg == WM_KEYDOWN) {
        const char* seq = nullptr;
        switch (wp) {
        case VK_UP: seq = "\x1b[A"; break;
        case VK_DOWN: seq = "\x1b[B"; break;
        case VK_RIGHT: seq = "\x1b[C"; break;
        case VK_LEFT: seq = "\x1b[D"; break;
        case VK_HOME: seq = "\x1b[H"; break;
        case VK_END: seq = "\x1b[F"; break;
        case VK_INSERT: seq = "\x1b[2~"; break;
        case VK_DELETE: seq = "\x1b[3~"; break;
        case VK_PRIOR: seq = "\x1b[5~"; break;
        case VK_NEXT: seq = "\x1b[6~"; break;
        default: return false;  // characters arrive as WM_CHAR
        }
        SendToProgram(seq);
        return true;
    }
    // WM_CHAR
    wchar_t ch = static_cast<wchar_t>(wp);
    if (ch == 3 && c.Call(SCI_GETSELECTIONEMPTY) == 0) {  // Ctrl+C with a selection copies instead of interrupting
        c.Call(SCI_COPY);
        return true;
    }
    if (ch == 0x16) {  // Ctrl+V: paste clipboard text as typed input
        std::wstring text;
        if (OpenClipboard(hwnd_)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    text = p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        for (size_t pos; (pos = text.find(L"\r\n")) != std::wstring::npos;) text.erase(pos + 1, 1);
        for (wchar_t& w : text) {
            if (w == L'\n') w = L'\r';
        }
        SendToProgram(Narrow(text));
        return true;
    }
    if (ch == 8) {
        SendToProgram("\x7f");
        return true;
    }
    if (ch >= 0xD800 && ch <= 0xDBFF) {  // first half of a surrogate pair
        pendingSurrogate_ = ch;
        return true;
    }
    std::wstring w;
    if (ch >= 0xDC00 && ch <= 0xDFFF && pendingSurrogate_) w += pendingSurrogate_;
    pendingSurrogate_ = 0;
    w += ch;
    SendToProgram(Narrow(w));  // Enter arrives as '\r', Ctrl+letters as control codes
    return true;
}

LRESULT CALLBACK MainWindow::ConsoleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && self->ConsoleKey(msg, wp)) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void MainWindow::Print(const std::string& utf8, Out style) {
    Editor& c = *console_;
    if (termActive_) {  // put the program's output so far above the message
        term_.Commit();
        RenderTerminal();
    }
    TrimConsole();
    c.Call(SCI_SETREADONLY, 0);
    Sci_Position len = c.Call(SCI_GETLENGTH);
    std::string text = utf8;
    // Normalise CRLF so the console does not show doubled line breaks.
    for (size_t p; (p = text.find("\r\n")) != std::string::npos;) text.erase(p, 1);
    c.Call(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.data()));
    c.Call(SCI_STARTSTYLING, len);
    c.Call(SCI_SETSTYLING, text.size(), static_cast<int>(style));
    c.Call(SCI_SETREADONLY, 1);
    if (termActive_) termStart_ = c.Call(SCI_GETLENGTH);
    c.Call(SCI_GOTOPOS, c.Call(SCI_GETLENGTH));
}

void MainWindow::ConsoleDoubleClick(Sci_Position pos) {
    Editor& c = *console_;
    Sci_Position line = c.Call(SCI_LINEFROMPOSITION, pos);
    Sci_Position start = c.Call(SCI_POSITIONFROMLINE, line), end = c.Call(SCI_GETLINEENDPOSITION, line);
    std::string text(reinterpret_cast<const char*>(c.Call(SCI_GETRANGEPOINTER, start, end - start)), end - start);
    std::wsmatch m;
    std::wstring w = Widen(text);
    if (!std::regex_search(w, m, std::wregex(L"^\\s*(.+?):(\\d+):"))) return;
    fs::path p = m[1].str();
    if (p.is_relative() && !projectRoot_.empty()) p = projectRoot_ / p;
    if (FileExists(p.wstring())) OpenFile(p, _wtoi(m[2].str().c_str()));
}

void MainWindow::RequestCheck(Editor* ed) {
    if (!ed || !settings_->liveCheck || !Toolchain::NasmAvailable(*settings_)) return;
    CheckJob job{ed->id(), ed->version(), ed->Text(), IncludeDirs(ed->file()), *settings_};
    {
        std::lock_guard<std::mutex> lock(checkMutex_);
        auto it = std::find_if(checkQueue_.begin(), checkQueue_.end(), [&](const CheckJob& j) { return j.editorId == job.editorId; });
        if (it != checkQueue_.end()) *it = std::move(job);
        else checkQueue_.push_back(std::move(job));
    }
    checkCv_.notify_one();
}

void MainWindow::CheckWorker() {
    for (;;) {
        CheckJob job;
        {
            std::unique_lock<std::mutex> lock(checkMutex_);
            checkCv_.wait(lock, [this] { return checkStop_ || !checkQueue_.empty(); });
            if (checkStop_) return;
            job = std::move(checkQueue_.front());
            checkQueue_.pop_front();
        }
        auto* done = new CheckDone{job.editorId, job.version,
                                   Toolchain::Check(job.settings, job.text, job.editorId, job.includes)};
        if (!PostMessageW(hwnd_, WM_APP_CHECK_DONE, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }
}

void MainWindow::RefreshToolStatus() {
    Settings copy = *settings_;
    HWND hwnd = hwnd_;
    std::thread([copy, hwnd] {
        std::wstring version = Toolchain::NasmVersion(copy);
        bool linker = Toolchain::LinkerAvailable(copy);
        std::wstring text;
        bool warn = true;
        if (version.empty()) text = L"NASM не найден — укажите путь в настройках (Ctrl+Alt+S)";
        else if (!linker) text = L"NASM " + version + L" · линкер не найден — проверьте настройки";
        else {
            text = L"NASM " + version + L" · " + fs::path(copy.linkerPath).filename().wstring();
            warn = false;
        }
        PostMessageW(hwnd, WM_APP_TOOL_STATUS, warn, reinterpret_cast<LPARAM>(new std::wstring(text)));
    }).detach();
}

// ---- completion -------------------------------------------------------------------------------------------------

// ---- find usages ------------------------------------------------------------------------------------------------

void MainWindow::ClearUsageHighlight() {
    if (!usageEditor_) return;
    bool alive = std::any_of(editors_.begin(), editors_.end(), [&](const auto& e) { return e.get() == usageEditor_; });
    if (alive) {
        usageEditor_->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorFind);
        usageEditor_->Call(SCI_INDICATORCLEARRANGE, 0, usageEditor_->Call(SCI_GETLENGTH));
    }
    usageEditor_ = nullptr;
}

// The user-defined name under the caret and, for a local label (.loop), the global label it belongs to.
bool MainWindow::SymbolUnderCaret(Editor* ed, Sci_Position pos, std::string& word, std::string& scope) {
    word = ed->WordAt(pos);
    while (!word.empty() && word.back() == '.') word.pop_back();
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, pos > 0 && ed->Call(SCI_GETCHARAT, pos) == ' ' ? pos - 1 : pos));
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0])) || style == T_REGISTER ||
        style == T_INSTRUCTION || style == T_KEYWORD || style == T_DIRECTIVE || style == T_COMMENT || style == T_STRING) {
        std::string tip = "Поставьте курсор на метку, константу или макрос";
        ed->Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(tip.c_str()));
        return false;
    }
    scope.clear();
    if (word[0] == '.' && word.rfind("..", 0) != 0) {
        int caretLine = static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, pos)) + 1;
        for (const auto& s : Completion::ScanSymbols(ed->Text())) {
            if (s.line > caretLine) break;
            if (s.kind == Completion::Kind::Label && !s.declaration && s.name[0] != '.') scope = s.name;
        }
    }
    return true;
}

// Texts to search: the current file, other open tabs (current text) and the project's source files on disk.
// A local label is searched only in its own file.
std::vector<MainWindow::UsageSource> MainWindow::UsageSources(Editor* ed, bool local) {
    std::vector<UsageSource> sources{{ed->file(), ed, ed->Text()}};
    if (local) return sources;
    std::set<std::wstring> seen;
    if (!ed->file().empty()) seen.insert(Debugger::Key(ed->file()));
    for (const auto& e : editors_) {
        if (e.get() == ed || e->file().empty()) continue;
        if (seen.insert(Debugger::Key(e->file())).second) sources.push_back({e->file(), e.get(), e->Text()});
    }
    static const std::set<std::wstring> kSourceExt = {L".asm", L".inc", L".mac", L".s", L".nasm"};
    for (const fs::path& p : treePaths_) {
        if (!kSourceExt.count(Lower(p.extension().wstring())) || !seen.insert(Debugger::Key(p)).second) continue;
        std::error_code ec;
        if (fs::file_size(p, ec) > 4'000'000 || ec) continue;
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        sources.push_back({p, nullptr, ss.str()});
    }
    return sources;
}

// Lists every use of the name under the caret in the console ("file:line: code", double-click jumps there)
// and highlights the uses in the current file.
void MainWindow::FindUsages(Editor* ed, Sci_Position pos) {
    std::string word, scope;
    if (!SymbolUnderCaret(ed, pos, word, scope)) return;
    bool local = word[0] == '.' && word.rfind("..", 0) != 0;
    std::vector<UsageSource> sources = UsageSources(ed, local);

    struct Hit {
        const UsageSource* source;
        Completion::Occurrence occ;
    };
    std::vector<Hit> hits;
    size_t files = 0;
    for (const UsageSource& src : sources) {
        auto found = Completion::FindOccurrences(src.text, word, scope);
        if (!found.empty()) files++;
        for (auto& o : found) hits.push_back({&src, std::move(o)});
    }

    // Highlight the uses in the current file.
    ClearUsageHighlight();
    usageEditor_ = ed;
    ed->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorFind);
    for (const Hit& h : hits) {
        if (h.source->editor != ed) continue;
        Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, h.occ.line - 1);
        ed->Call(SCI_INDICATORFILLRANGE, lineStart + h.occ.column, h.occ.length);
    }

    Sci_Position headerLine = console_->Call(SCI_GETLINECOUNT) - 1;
    std::string what = local && !scope.empty() ? word + "» в " + scope : word + "»";
    Print("\nИспользования «" + what + ": " + std::to_string(hits.size()) + " в " + std::to_string(files) +
              (files == 1 ? " файле" : " файлах") + " — двойной клик открывает место\n",
          Out::Info);
    for (const Hit& h : hits) {
        fs::path shown = h.source->file;
        if (!shown.empty() && IsUnder(shown, projectRoot_)) shown = NormalizePath(shown).lexically_relative(NormalizePath(projectRoot_));
        std::string where = (shown.empty() ? Narrow(ed->Title()) : Narrow(shown.wstring())) + ":" + std::to_string(h.occ.line) + ":";
        std::string code = h.occ.lineText;
        size_t first = code.find_first_not_of(" \t");
        code = first == std::string::npos ? std::string() : code.substr(first);
        Print(where + " ", Out::Input);
        Print(code + (h.occ.definition ? "" : "\n"), Out::Normal);
        if (h.occ.definition) Print("   ← определение\n", Out::Success);
    }
    if (hits.empty()) Print("ничего не найдено\n", Out::Warning);
    console_->Call(SCI_SETFIRSTVISIBLELINE, headerLine);
}

// F2: renames a label, constant or macro everywhere in the project. Changed files are left unsaved (and open),
// so every edit can be reviewed and undone.
void MainWindow::Rename(Editor* ed, Sci_Position pos) {
    std::string word, scope;
    if (!SymbolUnderCaret(ed, pos, word, scope)) return;
    bool local = word[0] == '.' && word.rfind("..", 0) != 0;
    if (local && scope.empty()) {
        ShowMessage(hwnd_, L"Локальная метка вне глобальной метки — переименуйте её вручную.", false);
        return;
    }
    auto answer = InputBox(hwnd_, L"Переименование", L"Новое имя для «" + Widen(word) + L"»:", Widen(word));
    if (!answer) return;
    std::string name = Narrow(Trim(*answer));
    if (name == word) return;

    std::wstring problem;
    if (!Completion::IsValidName(name)) {
        problem = L"«" + Widen(name) + L"» — недопустимое имя. Можно буквы, цифры, _ . ? @ $ # ~; начинается не с цифры.";
    } else if (Completion::IsReservedWord(name)) {
        problem = L"«" + Widen(name) + L"» — зарезервированное слово NASM (регистр, инструкция или директива).";
    } else if (local != (name[0] == '.' && name.rfind("..", 0) != 0)) {
        problem = local ? L"Локальная метка должна начинаться с точки." : L"Имя глобального символа не может начинаться с точки.";
    }
    std::vector<UsageSource> sources = UsageSources(ed, local);
    if (problem.empty()) {  // the new name must not already be defined (in the same scope for a local label)
        for (const UsageSource& src : sources) {
            for (const auto& s : Completion::ScanSymbols(src.text)) {
                if (s.name != name || s.declaration) continue;
                if (local && s.scope != scope) continue;
                std::wstring where = src.file.empty() ? ed->Title() : src.file.filename().wstring();
                problem = L"«" + Widen(name) + L"» уже определено: " + where + L", строка " + std::to_wstring(s.line) + L".";
                break;
            }
            if (!problem.empty()) break;
        }
    }
    if (!problem.empty()) {
        ShowMessage(hwnd_, problem);
        return;
    }

    // Open every affected file (from disk text first, so unchanged files stay closed), then edit the tab's text.
    std::vector<Editor*> targets;
    for (const UsageSource& src : sources) {
        if (Completion::FindOccurrences(src.text, word, scope, !local).empty()) continue;
        Editor* target = src.editor ? src.editor : OpenFile(src.file);
        if (target) targets.push_back(target);
    }
    size_t total = 0;
    for (Editor* target : targets) {
        auto found = Completion::FindOccurrences(target->Text(), word, scope, !local);
        target->Call(SCI_BEGINUNDOACTION);
        for (auto it = found.rbegin(); it != found.rend(); ++it) {  // back to front: earlier offsets stay valid
            std::string replacement = it->qualified && local ? scope + name : name;
            Sci_Position at = target->Call(SCI_POSITIONFROMLINE, it->line - 1) + it->column;
            target->Call(SCI_SETTARGETRANGE, at, at + it->length);
            target->Call(SCI_REPLACETARGET, replacement.size(), reinterpret_cast<sptr_t>(replacement.c_str()));
        }
        target->Call(SCI_ENDUNDOACTION);
        total += found.size();
    }
    for (size_t i = 0; i < editors_.size(); i++) {
        if (editors_[i].get() == ed) Activate(static_cast<int>(i));
    }
    InvalidateChrome();
    Print("\nПереименовано «" + word + "» → «" + name + "»: " + std::to_string(total) + " замен в " +
              std::to_string(targets.size()) + (targets.size() == 1 ? " файле" : " файлах") +
              ". Файлы не сохранены: Ctrl+Z — отменить в файле, «Сохранить всё» — сохранить.\n",
          Out::Success);
}

// ---- go to definition -------------------------------------------------------------------------------------------

// Symbols of every other source file of the project (open tabs use their current text), with their files.
std::vector<MainWindow::SymbolHit> MainWindow::ProjectSymbols(const Editor* current) {
    std::vector<SymbolHit> out;
    std::set<std::wstring> open;
    for (const auto& e : editors_) {
        if (!e->file().empty()) open.insert(Debugger::Key(e->file()));
        if (e.get() == current) continue;
        for (auto& sym : Completion::ScanSymbols(e->Text())) out.push_back({e.get(), e->file(), std::move(sym)});
    }
    static const std::set<std::wstring> kSourceExt = {L".asm", L".inc", L".mac", L".s", L".nasm"};
    for (const fs::path& p : treePaths_) {
        if (!kSourceExt.count(Lower(p.extension().wstring()))) continue;
        std::wstring key = Debugger::Key(p);
        if (open.count(key)) continue;
        std::error_code ec;
        auto time = fs::last_write_time(p, ec);
        if (ec || fs::file_size(p, ec) > 1'000'000) continue;
        auto it = symbolCache_.find(key);
        if (it == symbolCache_.end() || it->second.time != time) {
            std::ifstream in(p, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            it = symbolCache_.insert_or_assign(key, CachedSymbols{time, Completion::ScanSymbols(ss.str()), p}).first;
        }
        for (const auto& sym : it->second.symbols) out.push_back({nullptr, it->second.path, sym});
    }
    return out;
}

std::vector<Completion::Symbol> MainWindow::OtherFileSymbols(const Editor* current) {
    std::vector<Completion::Symbol> out;
    for (auto& hit : ProjectSymbols(current)) {
        if (!hit.symbol.name.empty() && hit.symbol.name[0] != '.') out.push_back(std::move(hit.symbol));  // locals stay in their file
    }
    return out;
}

// Ctrl+click on the file name of a %include line opens that file.
bool MainWindow::OpenIncludeAt(Editor* ed, Sci_Position pos) {
    Sci_Position line = ed->Call(SCI_LINEFROMPOSITION, pos);
    Sci_Position start = ed->Call(SCI_POSITIONFROMLINE, line), end = ed->Call(SCI_GETLINEENDPOSITION, line);
    std::string text(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, start, end - start)), end - start);
    size_t i = text.find_first_not_of(" \t");
    if (i == std::string::npos || text.size() - i < 8 || _strnicmp(text.c_str() + i, "%include", 8) != 0) return false;
    size_t open = text.find_first_of("\"'<", i + 8);
    if (open == std::string::npos) return false;
    size_t close = text.find(text[open] == '<' ? '>' : text[open], open + 1);
    if (close == std::string::npos) return false;
    Sci_Position col = pos - start;
    if (col < static_cast<Sci_Position>(open) || col > static_cast<Sci_Position>(close)) return false;
    fs::path name = Widen(text.substr(open + 1, close - open - 1));
    std::vector<fs::path> dirs;
    if (!ed->file().empty()) dirs.push_back(ed->file().parent_path());
    if (!projectRoot_.empty()) dirs.push_back(projectRoot_);
    for (const fs::path& d : dirs) {
        if (FileExists((d / name).wstring())) {
            PushNavigation();
            OpenFile(d / name);
            return true;
        }
    }
    std::string tip = "Файл не найден: " + Narrow(name.wstring());
    ed->Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(tip.c_str()));
    return true;
}

void MainWindow::GoToDefinition(Editor* ed, Sci_Position pos) {
    if (OpenIncludeAt(ed, pos)) return;
    std::string word = ed->WordAt(pos);
    while (!word.empty() && word.back() == '.') word.pop_back();
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0]))) return;
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, pos > 0 && ed->Call(SCI_GETCHARAT, pos) == ' ' ? pos - 1 : pos));
    if (style == T_REGISTER || style == T_INSTRUCTION || style == T_KEYWORD || style == T_COMMENT || style == T_STRING) {
        return;  // built-in words and text have no definition
    }
    int caretLine = static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, pos)) + 1;
    std::vector<Completion::Symbol> own = Completion::ScanSymbols(ed->Text());

    // Local label (.loop): belongs to the last global label above the cursor.
    bool local = word[0] == '.' && word.rfind("..", 0) != 0;
    std::string scope;
    size_t dot = word.find('.', 1);
    if (local) {
        for (const auto& s : own) {
            if (s.line > caretLine) break;
            if (s.kind == Completion::Kind::Label && !s.declaration && s.name[0] != '.') scope = s.name;
        }
    } else if (dot != std::string::npos && word.rfind("..", 0) != 0) {  // fully qualified: main.loop
        auto qualified = [&](const Completion::Symbol& s) {
            return s.name == word.substr(dot) && s.scope == word.substr(0, dot);
        };
        for (const auto& s : own) {
            if (qualified(s)) return JumpTo({ed, ed->file(), s});
        }
        for (const auto& hit : ProjectSymbols(ed)) {
            if (qualified(hit.symbol)) return JumpTo(hit);
        }
    }

    auto isDefinition = [&](const Completion::Symbol& s) {
        return s.name == word && !s.declaration && (s.kind == Completion::Kind::Label || s.kind == Completion::Kind::Macro);
    };
    if (local) {
        const Completion::Symbol* best = nullptr;
        for (const auto& s : own) {
            if (!isDefinition(s)) continue;
            if (s.scope == scope) {
                best = &s;
                break;
            }
            if (!best) best = &s;
        }
        if (best) return JumpTo({ed, ed->file(), *best});
    } else {
        for (const auto& s : own) {
            if (isDefinition(s)) return JumpTo({ed, ed->file(), s});
        }
        std::vector<SymbolHit> others = ProjectSymbols(ed);
        for (const auto& hit : others) {
            if (isDefinition(hit.symbol)) return JumpTo(hit);
        }
        // Not defined in the project: show where it is declared (extern/global).
        auto declared = [&](const Completion::Symbol& s) { return s.name == word && s.declaration; };
        const SymbolHit* decl = nullptr;
        SymbolHit ownDecl{};
        for (const auto& s : own) {
            if (declared(s)) {
                ownDecl = {ed, ed->file(), s};
                decl = &ownDecl;
                break;
            }
        }
        if (!decl) {
            for (const auto& hit : others) {
                if (declared(hit.symbol)) {
                    decl = &hit;
                    break;
                }
            }
        }
        if (decl) {
            JumpTo(*decl);
            Editor* now = Active();
            if (now && decl->symbol.kind == Completion::Kind::Function) {
                std::string tip = word + " — внешний символ: определён вне проекта (библиотека или DLL)";
                now->Call(SCI_CALLTIPSHOW, now->Call(SCI_GETCURRENTPOS), reinterpret_cast<sptr_t>(tip.c_str()));
            }
            return;
        }
    }
    std::string tip = "Определение «" + word + "» не найдено";
    ed->Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(tip.c_str()));
}

void MainWindow::JumpTo(const SymbolHit& hit) {
    PushNavigation();
    Editor* ed = nullptr;
    if (hit.editor) {
        for (size_t i = 0; i < editors_.size(); i++) {
            if (editors_[i].get() == hit.editor) Activate(static_cast<int>(i));
        }
        ed = hit.editor;
    } else {
        ed = OpenFile(hit.file);
    }
    if (!ed) return;
    ed->GotoLine(hit.symbol.line);
    // Select the name on its line.
    Sci_Position line = hit.symbol.line - 1;
    Sci_Position start = ed->Call(SCI_POSITIONFROMLINE, line), end = ed->Call(SCI_GETLINEENDPOSITION, line);
    std::string text(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, start, end - start)), end - start);
    size_t at = text.find(hit.symbol.name);
    if (at != std::string::npos) ed->Call(SCI_SETSEL, start + at, start + at + hit.symbol.name.size());
}

MainWindow::NavLocation MainWindow::CurrentLocation() {
    Editor* ed = Active();
    if (!ed) return {{}, 0, 0};
    return {ed->file(), ed->id(), ed->Call(SCI_GETCURRENTPOS)};
}

void MainWindow::PushNavigation() {
    if (!Active()) return;
    navBack_.push_back(CurrentLocation());
    if (navBack_.size() > 100) navBack_.erase(navBack_.begin());
    navForward_.clear();
}

// Alt+Left / Alt+Right: back and forward through the places visited by "go to definition".
void MainWindow::Navigate(bool back) {
    auto& from = back ? navBack_ : navForward_;
    auto& to = back ? navForward_ : navBack_;
    while (!from.empty()) {
        NavLocation loc = from.back();
        from.pop_back();
        Editor* ed = nullptr;
        if (!loc.file.empty() && FileExists(loc.file.wstring())) {
            ed = OpenFile(loc.file);
        } else if (Editor* e = FindById(loc.editorId)) {
            for (size_t i = 0; i < editors_.size(); i++) {
                if (editors_[i].get() == e) Activate(static_cast<int>(i));
            }
            ed = e;
        }
        if (!ed) continue;  // the tab was closed: try the next entry
        if (Active()) to.push_back(CurrentLocation());
        ed->Call(SCI_GOTOPOS, std::min<Sci_Position>(loc.pos, ed->Call(SCI_GETLENGTH)));
        ed->Call(SCI_SCROLLCARET);
        SetFocus(ed->hwnd());
        return;
    }
    MessageBeep(MB_OK);
}

void MainWindow::ClearLinkHover() {
    if (!linkEditor_) return;
    bool alive = std::any_of(editors_.begin(), editors_.end(), [&](const auto& e) { return e.get() == linkEditor_; });
    if (alive) {  // the tab may have been closed meanwhile
        linkEditor_->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorLink);
        linkEditor_->Call(SCI_INDICATORCLEARRANGE, linkStart_, linkEnd_ - linkStart_);
    }
    linkEditor_ = nullptr;
}

// Ctrl held over a name: underline it like a link.
void MainWindow::UpdateLinkHover(Editor* ed, bool ctrl) {
    Sci_Position start = 0, end = 0;
    bool link = false;
    if (ctrl) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(ed->hwnd(), &pt);
        Sci_Position pos = ed->Call(SCI_POSITIONFROMPOINTCLOSE, pt.x, pt.y);
        if (pos >= 0 && ed->WordRange(pos, start, end)) {
            int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, start));
            char first = static_cast<char>(ed->Call(SCI_GETCHARAT, start));
            link = (style == T_DEFAULT || style == T_LABEL) && !isdigit(static_cast<unsigned char>(first));
            if (!link && style == T_STRING) {  // file name in %include "..."
                Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, ed->Call(SCI_LINEFROMPOSITION, pos));
                link = ed->Call(SCI_GETSTYLEAT, ed->Call(SCI_GETLINEINDENTPOSITION, ed->Call(SCI_LINEFROMPOSITION, lineStart))) ==
                       T_PREPROCESSOR;
            }
        }
    }
    if (link && linkEditor_ == ed && linkStart_ == start && linkEnd_ == end) return;
    ClearLinkHover();
    if (!link) return;
    linkEditor_ = ed;
    linkStart_ = start;
    linkEnd_ = end;
    ed->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorLink);
    ed->Call(SCI_INDICATORFILLRANGE, start, end - start);
}

LRESULT CALLBACK MainWindow::EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    Editor* ed = self->FindByHwnd(hwnd);
    if (ed) {
        switch (msg) {
        case WM_MOUSEMOVE:
            self->UpdateLinkHover(ed, (wp & MK_CONTROL) != 0);
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
            if (wp == VK_CONTROL) self->UpdateLinkHover(ed, msg == WM_KEYDOWN);
            break;
        case WM_KILLFOCUS:
        case WM_MOUSELEAVE:
            self->ClearLinkHover();
            break;
        case WM_SETCURSOR:
            if (self->linkEditor_ == ed && LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            if (wp & MK_CONTROL) {  // Ctrl+click: go to definition (instead of Scintilla's word selection)
                Sci_Position pos = ed->Call(SCI_POSITIONFROMPOINTCLOSE, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                self->ClearLinkHover();
                if (pos >= 0) self->GoToDefinition(ed, pos);
                return 0;
            }
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Shows (or refreshes) the completion list for the word before the caret.
void MainWindow::Complete(Editor* ed, bool explicitRequest) {
    Sci_Position pos = ed->Call(SCI_GETCURRENTPOS);
    Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, ed->Call(SCI_LINEFROMPOSITION, pos));
    std::string line(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, lineStart, pos - lineStart)),
                     pos - lineStart);
    size_t start = line.size();
    auto wordChar = [](unsigned char c) {
        return isalnum(c) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' || c == '#' || c == '~' || c >= 0x80;
    };
    while (start > 0 && wordChar(static_cast<unsigned char>(line[start - 1]))) start--;
    if (start > 0 && line[start - 1] == '%') start--;
    std::string prefix = line.substr(start);
    bool active = ed->Call(SCI_AUTOCACTIVE) != 0;
    if (!explicitRequest && !active && prefix.size() < 2) return;  // pops up by itself after two characters
    if (!prefix.empty() && isdigit(static_cast<unsigned char>(prefix[0]))) return;

    std::vector<Completion::Symbol> symbols = Completion::ScanSymbols(ed->Text());
    for (auto& sym : OtherFileSymbols(ed)) symbols.push_back(std::move(sym));
    std::string list = Completion::BuildList(line.substr(0, start), prefix, symbols);
    if (list.empty()) {
        if (active) ed->Call(SCI_AUTOCCANCEL);
        return;
    }
    ed->Call(SCI_AUTOCSHOW, prefix.size(), reinterpret_cast<sptr_t>(list.c_str()));
    // The list is a popup created by Scintilla: give it (and its scrollbar) the dark theme.
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM) -> BOOL {
        wchar_t cls[32];
        if (GetClassNameW(w, cls, 32) && wcscmp(cls, L"ListBoxX") == 0 && !GetPropW(w, L"AsmDark")) {
            SetPropW(w, L"AsmDark", reinterpret_cast<HANDLE>(1));
            ApplyDarkControl(w);
            EnumChildWindows(w, [](HWND child, LPARAM) -> BOOL {
                ApplyDarkControl(child);
                return TRUE;
            }, 0);
        }
        return TRUE;
    }, 0);
}

// ---- find & replace ---------------------------------------------------------------------------------------------

void MainWindow::ShowFind(bool replace) {
    Editor* ed = Active();
    if (ed) {
        Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
        if (e > s && e - s < 200) {
            std::string sel(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, s, e - s)), e - s);
            if (sel.find('\n') == std::string::npos) wcsncpy_s(findText_, Widen(sel).c_str(), _TRUNCATE);
        }
    }
    if (findDialog_) DestroyWindow(findDialog_);
    findData_ = {sizeof(findData_)};
    findData_.hwndOwner = hwnd_;
    findData_.lpstrFindWhat = findText_;
    findData_.wFindWhatLen = static_cast<WORD>(std::size(findText_));
    findData_.lpstrReplaceWith = replaceText_;
    findData_.wReplaceWithLen = static_cast<WORD>(std::size(replaceText_));
    findData_.Flags = lastFindFlags_ & (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    findDialog_ = replace ? ReplaceTextW(&findData_) : FindTextW(&findData_);
}

bool MainWindow::FindNext(bool forward) {
    Editor* ed = Active();
    if (!ed || lastFind_.empty()) return false;
    std::string q = Narrow(lastFind_);
    int flags = (lastFindFlags_ & FR_MATCHCASE ? SCFIND_MATCHCASE : 0) | (lastFindFlags_ & FR_WHOLEWORD ? SCFIND_WHOLEWORD : 0);
    ed->Call(SCI_SETSEARCHFLAGS, flags);
    Sci_Position len = ed->Call(SCI_GETLENGTH);
    auto search = [&](Sci_Position from, Sci_Position to) {
        ed->Call(SCI_SETTARGETRANGE, from, to);
        return ed->Call(SCI_SEARCHINTARGET, q.size(), reinterpret_cast<sptr_t>(q.c_str()));
    };
    Sci_Position r = forward ? search(ed->Call(SCI_GETSELECTIONEND), len) : search(ed->Call(SCI_GETSELECTIONSTART), 0);
    if (r < 0) r = forward ? search(0, len) : search(len, 0);
    if (r < 0) {
        MessageBeep(MB_ICONASTERISK);
        return false;
    }
    ed->Call(SCI_SETSEL, ed->Call(SCI_GETTARGETSTART), ed->Call(SCI_GETTARGETEND));
    ed->Call(SCI_SCROLLCARET);
    return true;
}

void MainWindow::OnFindMessage(FINDREPLACEW* fr) {
    if (fr->Flags & FR_DIALOGTERM) {
        findDialog_ = nullptr;
        return;
    }
    lastFind_ = fr->lpstrFindWhat;
    lastFindFlags_ = fr->Flags;
    Editor* ed = Active();
    if (!ed) return;
    if (fr->Flags & FR_FINDNEXT) {
        FindNext((fr->Flags & FR_DOWN) != 0);
    } else if (fr->Flags & FR_REPLACE) {
        std::string q = Narrow(lastFind_), rep = Narrow(fr->lpstrReplaceWith);
        Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
        std::string sel(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, s, e - s)), e - s);
        bool match = (fr->Flags & FR_MATCHCASE) ? sel == q : _stricmp(sel.c_str(), q.c_str()) == 0;
        if (match && !sel.empty()) ed->Call(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(rep.c_str()));
        FindNext(true);
    } else if (fr->Flags & FR_REPLACEALL) {
        std::string q = Narrow(lastFind_), rep = Narrow(fr->lpstrReplaceWith);
        int flags = (fr->Flags & FR_MATCHCASE ? SCFIND_MATCHCASE : 0) | (fr->Flags & FR_WHOLEWORD ? SCFIND_WHOLEWORD : 0);
        ed->Call(SCI_SETSEARCHFLAGS, flags);
        ed->Call(SCI_BEGINUNDOACTION);
        int count = 0;
        Sci_Position pos = 0;
        for (;;) {
            ed->Call(SCI_SETTARGETRANGE, pos, ed->Call(SCI_GETLENGTH));
            if (ed->Call(SCI_SEARCHINTARGET, q.size(), reinterpret_cast<sptr_t>(q.c_str())) < 0) break;
            Sci_Position n = ed->Call(SCI_REPLACETARGET, rep.size(), reinterpret_cast<sptr_t>(rep.c_str()));
            pos = ed->Call(SCI_GETTARGETSTART) + n;
            count++;
        }
        ed->Call(SCI_ENDUNDOACTION);
        ShowMessage(findDialog_ ? findDialog_ : hwnd_, L"Заменено: " + std::to_wstring(count), false);
    }
}
