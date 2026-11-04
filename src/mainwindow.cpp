#include "mw_internal.h"
#include "version.h"
#include "hints.h"
#include "project.h"

namespace {

// Scintilla creates the completion list (ListBoxX and its list box) on first use; give the new windows the theme
// before they are shown, so the first popup does not flash a light scrollbar.
HHOOK g_cbtHook = nullptr;

LRESULT CALLBACK CbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp);
        HWND hwnd = reinterpret_cast<HWND>(wp);
        wchar_t cls[32] = L"", parentCls[32] = L"";
        GetClassNameW(hwnd, cls, 32);
        if (create->lpcs->hwndParent) GetClassNameW(create->lpcs->hwndParent, parentCls, 32);
        if (wcscmp(cls, L"ListBoxX") == 0 || wcscmp(parentCls, L"ListBoxX") == 0) ApplyDarkControl(hwnd);
        if (wcscmp(parentCls, L"ListBoxX") == 0) ScrollBars::AttachLater(hwnd);  // its list: our scrollbar
    }
    return CallNextHookEx(g_cbtHook, code, wp, lp);
}
}

// ---- creation ---------------------------------------------------------------------------------------------------

bool MainWindow::Create(HINSTANCE inst, Settings* settings) {
    inst_ = inst;
    settings_ = settings;
    findMsg_ = RegisterWindowMessageW(FINDMSGSTRINGW);
    debugStatusColor_ = Theme::Muted;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"AsmEditorMain";
    RegisterClassExW(&wc);

    WNDCLASSW bar{};
    bar.lpfnWndProc = MarkBarProc;
    bar.hInstance = inst;
    bar.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bar.lpszClassName = L"AsmMarkBar";
    RegisterClassW(&bar);

    g_cbtHook = SetWindowsHookExW(WH_CBT, CbtProc, nullptr, GetCurrentThreadId());

    const RECT& w = settings->window;
    bool hasPos = w.right > 200 && w.bottom > 200;
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, L"AsmEditorMain", L"AsmEditor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            hasPos ? w.left : CW_USEDEFAULT, hasPos ? w.top : CW_USEDEFAULT,
                            hasPos ? w.right : 1280, hasPos ? w.bottom : 820, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    if (!hasPos) {
        // Default size was given in 96-dpi pixels; scale it to the monitor.
        SetWindowPos(hwnd_, nullptr, 0, 0, S(1280), S(820), SWP_NOMOVE | SWP_NOZORDER);
    }
    ApplyDarkWindow(hwnd_, Theme::FrameBg);
    BuildAccelerators();

    checkThread_ = std::thread([this] { CheckWorker(); });
    RefreshToolStatus();
    SetTimer(hwnd_, kBackupTimer, 30'000, nullptr);
    SetTimer(hwnd_, kUpdateTimer, 4'000, nullptr);  // the update check, once the window is up
    Log::Write("AsmEditor started");

    ShowWindow(hwnd_, settings->maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd_);
    RestoreDebugLayout();  // the parts of the debugger where they were left (they show up while debugging)
    // A teacher's task files loaded earlier (Run → Load a teacher's tasks).
    std::error_code ec;
    for (fs::directory_iterator it(Settings::Dir() / L"tasks", ec), end; !ec && it != end; it.increment(ec)) {
        std::ifstream in(it->path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::vector<std::string> errors;
        Exercises::AddCustom(Exercises::ParseTasks(ss.str(), errors));
    }
    return true;
}

void MainWindow::CreateFonts() {
    if (uiFont_) DeleteObject(uiFont_);
    if (uiBold_) DeleteObject(uiBold_);
    if (titleFont_) DeleteObject(titleFont_);
    uiFont_ = CreateUiFont(dpi_, 9);
    uiBold_ = CreateUiFont(dpi_, 9, true);
    titleFont_ = CreateUiFont(dpi_, 22, true);
    for (HWND h : {tree_, outline_, memExpr_, watchExpr_}) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
}

void MainWindow::CreateChildren() {
    dpi_ = GetDpiForWindow(hwnd_);
    inputBrush_ = CreateSolidBrush(Theme::InputBg);

    auto makeTree = [&](int id, DWORD extra) {
        HWND t = CreateWindowExW(0, WC_TREEVIEWW, L"",
                                 WS_CHILD | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_TRACKSELECT | TVS_NOHSCROLL |
                                     TVS_INFOTIP | extra,
                                 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        ApplyDarkControl(t);
        TreeView_SetExtendedStyle(t, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        TreeView_SetBkColor(t, Theme::EditorBg);
        TreeView_SetTextColor(t, Theme::Text);
        ScrollBars::Attach(t);
        return t;
    };
    tree_ = makeTree(ID_TREE_CTRL, TVS_HASBUTTONS);
    ShowWindow(tree_, SW_SHOW);
    TreeView_SetImageList(tree_, FileIcons::ImageList(S(16)), TVSIL_NORMAL);
    outline_ = makeTree(ID_OUTLINE_CTRL, TVS_HASBUTTONS | TVS_LINESATROOT);

    for (auto& view : views_) {
        view = std::make_unique<Editor>(hwnd_, settings_->fontSize);
        SetupOutputView(*view);
    }
    SetWindowSubclass(View(Pane::Terminal).hwnd(), ConsoleProc, 3, reinterpret_cast<DWORD_PTR>(this));
    ShowPane(Pane::Output);

    for (auto* v : {&regsView_, &memView_, &stackView_, &watchView_, &disasmView_, &flagsView_}) {
        *v = std::make_unique<Editor>(hwnd_, settings_->fontSize);
        SetupPanelView(**v);
    }
    auto makeInput = [&](int id, const wchar_t* cue) {
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        SendMessageW(e, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        SetWindowSubclass(e, InputProc, id, reinterpret_cast<DWORD_PTR>(this));
        return e;
    };
    memExpr_ = makeInput(ID_MEM_EXPR, Tr(L"адрес: rsp, msg, rbx+8, [rsp]… Enter"));
    SetWindowTextW(memExpr_, L"rsp");
    watchExpr_ = makeInput(ID_WATCH_EXPR, Tr(L"добавить: rax, [rsp+8], dword [counter], rcx*2… Enter"));
    markBar_ = CreateWindowExW(0, L"AsmMarkBar", L"", WS_CHILD, 0, 0, 10, 10, hwnd_, nullptr, inst_, this);
    markBar2_ = CreateWindowExW(0, L"AsmMarkBar", L"", WS_CHILD, 0, 0, 10, 10, hwnd_, nullptr, inst_, this);

    CreateFonts();
    for (HWND e : {memExpr_, watchExpr_}) SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
    BuildMenus();

    tools_ = {
        {ID_MENU_FILE, L"Файл", 0, true, false},
        {ID_MENU_EDIT, L"Правка", 0, true, false},
        {ID_MENU_VIEW, L"Вид", 0, true, false},
        {ID_MENU_RUN, L"Запуск", 0, true, false},
        {ID_MENU_HELP, L"Справка", 0, true, false},
        {ID_PROJECT_WIDGET, L"", 0, false, false},
        {ID_BUILD, L"Собрать", 1, false, false},
        {ID_RUN, L"Запустить", 2, false, false, 2},
        {ID_DEBUG, L"Отладка", 6, false, false, 2},
        {ID_DEBUG, L"Продолжить", 2, false, false, 1},
        {ID_PAUSE, L"Пауза", 7, false, false, 1},
        {ID_STEP_OVER, L"Шаг", 8, false, false, 1},
        {ID_STEP_INTO, L"Войти", 9, false, false, 1},
        {ID_STEP_OUT, L"Выйти", 10, false, false, 1},
        {ID_STOP, L"Стоп", 3, false, false},
        {ID_TARGET, L"", 11, false, false},
        {ID_PROFILE, L"", 0, false, false},
        {ID_SETTINGS, L"Настройки", 4, false, true},
    };
}

// A read-only view of the bottom panel: message styles (Out) and the ANSI colors of the terminal.
void MainWindow::SetupOutputView(Editor& c) {
    c.Call(SCI_SETMARGINWIDTHN, 0, 0);
    c.Call(SCI_SETMARGINWIDTHN, 1, 0);
    c.Call(SCI_SETMARGINWIDTHN, Editor::kFoldMargin, 0);
    c.Call(SCI_SETMARGINWIDTHN, Editor::kGapMargin, 6);
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
    for (int i = 0; i < 16; i++) {
        c.Call(SCI_STYLESETFORE, Terminal::kStyleBase + i, Theme::Ansi[i]);
        c.Call(SCI_STYLESETITALIC, Terminal::kStyleBase + i, 0);
    }
    c.Call(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, static_cast<sptr_t>(Theme::Bright) | (0xFFll << 24));
}

void MainWindow::SetupPanelView(Editor& v) {
    v.Call(SCI_SETMARGINWIDTHN, 0, 0);
    v.Call(SCI_SETMARGINWIDTHN, 1, 0);
    v.Call(SCI_SETMARGINWIDTHN, Editor::kFoldMargin, 0);
    v.Call(SCI_SETMARGINWIDTHN, Editor::kGapMargin, 4);
    v.Call(SCI_SETCARETLINEVISIBLEALWAYS, 0);
    v.Call(SCI_SETCARETLINEVISIBLE, 0);
    v.Call(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    v.Call(SCI_SETREADONLY, 1);
    v.Call(SCI_SETUNDOCOLLECTION, 0);
    v.Call(SCI_SETMOUSEDWELLTIME, SC_TIME_FOREVER);
    ScrollBars::Show(v.hwnd(), true, false);
    const std::pair<int, COLORREF> colors[] = {
        {P_TEXT, Theme::Text}, {P_LABEL, Theme::Register}, {P_CHANGED, Theme::Changed}, {P_HEADER, Theme::Muted},
        {P_SYMBOL, Theme::Link}, {P_FLAG_ON, Theme::Success}, {P_DIM, Theme::Dim}, {P_ERROR, Theme::Error},
        {P_ADDR, Theme::Muted}, {P_CURRENT, Theme::ExecArrow}, {P_CODE, Theme::Instruction},
        {P_YES, Theme::Success}, {P_NO, Theme::Error}};
    for (auto [style, color] : colors) {
        v.Call(SCI_STYLESETFORE, style, color);
        v.Call(SCI_STYLESETITALIC, style, 0);
        v.Call(SCI_STYLESETBOLD, style, style == P_HEADER || style == P_CHANGED || style == P_CURRENT || style == P_YES);
        v.Call(SCI_STYLESETSIZE, style, std::max(6, settings_->fontSize - 1));
    }
    // Indicator 9: the fading highlight of stack slots that changed at this step (registers view).
    v.Call(SCI_INDICSETSTYLE, 9, INDIC_ROUNDBOX);
    v.Call(SCI_INDICSETFORE, 9, Theme::Changed);
    v.Call(SCI_INDICSETUNDER, 9, 1);
}

std::vector<Editor*> MainWindow::PanelViews() const {
    std::vector<Editor*> out;
    for (auto* v : {&regsView_, &memView_, &stackView_, &watchView_, &disasmView_, &flagsView_}) {
        if (*v) out.push_back(v->get());
    }
    return out;
}

void MainWindow::BuildMenus() {
    fileMenu_ = CreatePopupMenu();
    recentMenu_ = CreatePopupMenu();
    AppendCommand(fileMenu_, ID_NEW);
    AppendCommand(fileMenu_, ID_NEW_PROJECT);
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendCommand(fileMenu_, ID_OPEN_FILE);
    AppendCommand(fileMenu_, ID_OPEN_FOLDER);
    AppendCommand(fileMenu_, ID_QUICK_OPEN);
    AppendCommand(fileMenu_, ID_SEARCH_EVERYWHERE);
    AppendCommand(fileMenu_, ID_RECENT_FILES);
    AppendMenuW(fileMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(recentMenu_), Tr(L"Недавние проекты"));
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendCommand(fileMenu_, ID_SAVE);
    AppendCommand(fileMenu_, ID_SAVE_AS);
    AppendCommand(fileMenu_, ID_SAVE_ALL);
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendCommand(fileMenu_, ID_CLOSE_TAB);
    AppendCommand(fileMenu_, ID_CLOSE_OTHERS);
    AppendCommand(fileMenu_, ID_CLOSE_ALL);
    AppendCommand(fileMenu_, ID_CLOSE_PROJECT);
    AppendMenuW(fileMenu_, MF_SEPARATOR, 0, nullptr);
    AppendCommand(fileMenu_, ID_EXIT);

    editMenu_ = CreatePopupMenu();
    AppendCommands(editMenu_, {ID_UNDO, ID_REDO, 0});
    AppendSubmenu(editMenu_, L"Поиск", {ID_FIND, ID_REPLACE, 0, ID_FIND_NEXT, ID_FIND_PREV, 0, ID_FIND_IN_FILES});
    AppendSubmenu(editMenu_, L"Навигация", {ID_GOTO, ID_GOTO_DEF, ID_FIND_USAGES, 0, ID_NAV_BACK, ID_NAV_FORWARD});
    AppendSubmenu(editMenu_, L"Код", {ID_COMPLETE, ID_SNIPPET, ID_SHOW_HINT, ID_QUICK_FIX, 0, ID_RENAME, ID_FORMAT,
                                      ID_COMMENT, ID_DUPLICATE});
    AppendSubmenu(editMenu_, L"Несколько курсоров", {ID_ADD_CARET_UP, ID_ADD_CARET_DOWN, 0, ID_SELECT_NEXT,
                                                     ID_SELECT_ALL_OCCURRENCES});
    AppendSubmenu(editMenu_, L"Сворачивание", {ID_TOGGLE_FOLD, ID_FOLD_ALL, ID_UNFOLD_ALL});

    viewMenu_ = CreatePopupMenu();
    themeMenu_ = CreatePopupMenu();
    for (size_t i = 0; i < Theme::All().size(); i++) {
        const Theme::Info& t = Theme::All()[i];
        AppendMenuW(themeMenu_, MF_STRING | (settings_->theme == t.id ? MF_CHECKED : 0), ID_THEME + i, Tr(t.title));
    }
    AppendCommands(viewMenu_, {ID_COMMAND_PALETTE, 0});
    AppendSubmenu(viewMenu_, L"Окна инструментов", {ID_OUTLINE, ID_DEBUG_PANEL, ID_DETACH_DEBUG, 0, ID_SHOW_OUTPUT,
                                                    ID_SHOW_TERMINAL, ID_SHOW_PROBLEMS});
    AppendSubmenu(viewMenu_, L"Вкладки", {ID_NEXT_TAB, ID_PREV_TAB, 0, ID_SPLIT, ID_SPLIT_DOWN, ID_UNSPLIT, 0,
                                          ID_DETACH_TAB, ID_PIN_TAB});
    AppendSubmenu(viewMenu_, L"Масштаб", {ID_ZOOM_IN, ID_ZOOM_OUT, ID_ZOOM_RESET});
    AppendMenuW(viewMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(viewMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu_), Tr(L"Тема"));
    AppendCommand(viewMenu_, ID_LANGUAGE);
    AppendCommand(viewMenu_, ID_INLAY_HINTS);
    CheckMenuItem(viewMenu_, ID_DEBUG_PANEL, showDebugPanel_ ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(viewMenu_, ID_INLAY_HINTS, settings_->inlayHints ? MF_CHECKED : MF_UNCHECKED);

    runMenu_ = CreatePopupMenu();
    AppendCommands(runMenu_, {ID_BUILD, ID_REBUILD, 0, ID_RUN, ID_DEBUG, ID_RUN_TO_CURSOR, ID_STOP, 0});
    AppendSubmenu(runMenu_, L"Пошаговое выполнение", {ID_STEP_OVER, ID_STEP_INTO, ID_STEP_OUT, ID_STEP_BACK, 0,
                                                      ID_SET_NEXT, ID_PAUSE, 0, ID_ATTACH});
    AppendSubmenu(runMenu_, L"Точки останова", {ID_TOGGLE_BP, ID_BP_CONDITION, ID_DATA_BP, ID_ADD_WATCH, 0,
                                                ID_CLEAR_BPS});
    AppendSubmenu(runMenu_, L"Профилирование", {ID_PROFILE_RUN, ID_PROFILE_SAMPLE, 0, ID_PROFILE_CLEAR});
    AppendSubmenu(runMenu_, L"Конфигурация", {ID_CHOOSE_TARGET, ID_RUN_CONFIG, 0, ID_PROFILE_DEBUG,
                                              ID_PROFILE_RELEASE, 0, ID_RESET_MAIN});
    AppendSubmenu(runMenu_, L"Упражнения", {ID_LESSONS, ID_NEW_EXERCISE, ID_EXERCISE_HINT, ID_CHECK_SOLUTION, 0,
                                            ID_LOAD_TASKS, ID_EXPORT_RESULTS, ID_GROUP_SUMMARY});
    AppendCommands(runMenu_, {0, ID_CLEAR_CONSOLE, ID_SETTINGS});

    helpMenu_ = CreatePopupMenu();
    AppendCommands(helpMenu_, {ID_HELP_PAGE, ID_COMMAND_PALETTE, ID_KEYS, 0, ID_UPDATE_APP, ID_ASSOCIATE,
                               ID_CHECK_UPDATES, ID_OPEN_LOG, 0, ID_ABOUT});
}

bool MainWindow::PreTranslate(MSG& msg) {
    if (findDialog_ && IsDialogMessageW(findDialog_, &msg)) return true;
    bool ours = msg.hwnd == hwnd_ || IsChild(hwnd_, msg.hwnd) || IsFloatWindow(GetAncestor(msg.hwnd, GA_ROOT));
    // Shift pressed twice on its own: Search Everywhere, as in IntelliJ.
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
        if (msg.wParam != VK_SHIFT) {
            shiftAlone_ = false;
            lastShiftTap_ = 0;
        } else if (!(msg.lParam & 0x40000000)) {
            shiftAlone_ = true;
        }
    } else if (msg.message == WM_KEYUP && msg.wParam == VK_SHIFT) {
        if (shiftAlone_ && ours) {
            DWORD now = GetTickCount();
            if (lastShiftTap_ && now - lastShiftTap_ < 400) {
                lastShiftTap_ = 0;
                PostMessageW(hwnd_, WM_COMMAND, ID_SEARCH_EVERYWHERE, 0);
            } else {
                lastShiftTap_ = now;
            }
        }
        shiftAlone_ = false;
    } else if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN) {
        lastShiftTap_ = 0;
    }
    if (ours) {
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

// Enter in the memory address / watch input rows.
LRESULT CALLBACK MainWindow::InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (id == ID_MEM_EXPR) {
            self->RenderMemory();
        } else if (id == ID_WATCH_EXPR) {
            int n = GetWindowTextLengthW(hwnd);
            std::wstring text(n, L'\0');
            GetWindowTextW(hwnd, text.data(), n + 1);
            if (!Trim(text).empty()) self->AddWatch(Narrow(Trim(text)));
            SetWindowTextW(hwnd, L"");
        }
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == ScrollBars::MovedMessage()) {  // a frame of smooth scrolling: the mark bar's thumb follows at once
        HWND moved = reinterpret_cast<HWND>(wp);
        for (auto [bar, ed] : {std::pair{markBar_, Active()}, std::pair{markBar2_, SplitEditor()}}) {
            if (ed && ed->hwnd() == moved) RedrawWindow(bar, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
        return 0;
    }
    if (msg == findMsg_ && findMsg_) {
        OnFindMessage(reinterpret_cast<FINDREPLACEW*>(lp));
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        CreateChildren();
        return 0;
    case WM_SIZE:
        Tip::Hide();
        Layout();
        return 0;
    case WM_MOVE:
        Tip::Hide();
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        // Only the part to paint, in a bitmap of its size: the status bar alone (each frame of scrolling) is cheap.
        const RECT& r = ps.rcPaint;
        int w = std::max(1L, r.right - r.left), h = std::max(1L, r.bottom - r.top);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        SetViewportOrgEx(mem, -r.left, -r.top, nullptr);
        IntersectClipRect(mem, r.left, r.top, r.right, r.bottom);
        Paint(mem);
        BitBlt(dc, r.left, r.top, w, h, mem, r.left, r.top, SRCCOPY);
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
            if (drag_ == Drag::None && CrumbAt(p.x, p.y) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
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
            if (hoverLink_ >= 0 || hoverClear_ || hoverRefresh_ || hoverPane_ >= 0 || hoverLeftTab_ >= 0 ||
                (hoverMemFormat_ && PartHost(kMemoryPart) == hwnd_)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        hoverTool_ = hoverTab_ = hoverClose_ = hoverLink_ = hoverPane_ = hoverLeftTab_ = hoverPart_ = -1;
        hoverClear_ = hoverRefresh_ = hoverMemFormat_ = false;
        hoverCrumb_ = -1;
        InvalidateChrome();
        InvalidateRect(hwnd_, &rcStatus_, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (Contains(rcSplitV_, x, y)) drag_ = Drag::Tree;
        else if (Contains(rcSplitH_, x, y)) drag_ = Drag::Console;
        else if (DebugPanelVisible() && Contains(rcSplitD_, x, y)) drag_ = Drag::Debug;
        if (drag_ != Drag::None) {
            SetCapture(hwnd_);
            return 0;
        }
        if (TabMouseDown(hwnd_, x, y)) return 0;  // a tab (file or part): show it now, drag to move it
        if (int crumb = CrumbAt(x, y); crumb >= 0) {  // a breadcrumb: go to its line
            if (Editor* ed = Current()) ed->GotoLine(crumbs_[crumb].line + 1);
            return 0;
        }
        OnClick(x, y, false);
        return 0;
    }
    case WM_LBUTTONUP:
        if (drag_ == Drag::Tab) {
            TabDrop(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        } else if (drag_ != Drag::None) {
            drag_ = Drag::None;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (drag_ == Drag::Tab && reinterpret_cast<HWND>(lp) != hwnd_) {  // the drag was cancelled
            drag_ = Drag::None;
            dragTab_ = dragPart_ = -1;
            ShowDropHint(nullptr);
            InvalidateTabs();
        }
        break;
    case WM_ACTIVATE:
        // Back in the main window: commands act on its editors again.
        if (LOWORD(wp) != WA_INACTIVE) floatFocus_ = 0;
        break;
    case WM_MBUTTONUP:
        OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
        return 0;
    case WM_RBUTTONUP:
        OnRightClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &p);
        for (int group : {0, 1}) {
            if (!Contains(StripRect(group), p.x, p.y)) continue;
            int& scroll = tabScroll_[group];
            scroll = std::max(0, scroll - GET_WHEEL_DELTA_WPARAM(wp) / 2);
            InvalidateTabs();
            return 0;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_MEM_EXPR || LOWORD(wp) == ID_WATCH_EXPR) {
            // The frame around the input shows the focus (in whichever window holds it).
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                InvalidateRect(GetParent(reinterpret_cast<HWND>(lp)), nullptr, FALSE);
            }
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
            if (debugging_ && !DebugStopped() && termActive_) {
                ShowPane(Pane::Terminal);
                SetFocus(View(Pane::Terminal).hwnd());
            }
            return 0;
        }
        if (wp == kUpdateTimer) {
            KillTimer(hwnd_, wp);
            MaybeCheckForUpdate();
            return 0;
        }
        if (wp == kMenuHoverTimer) {
            KillTimer(hwnd_, wp);
            if (hoverTool_ >= 0 && tools_[hoverTool_].menu && drag_ == Drag::None && GetForegroundWindow() == hwnd_ &&
                !(GetKeyState(VK_LBUTTON) & 0x8000)) {
                OpenToolMenu(hoverTool_, true);
            }
            return 0;
        }
        if (wp == kMenuLeaveTimer) {
            menuAwayTicks_ = MouseInToolMenu() ? 0 : menuAwayTicks_ + 1;
            if (menuAwayTicks_ >= 3) EndMenu();
            return 0;
        }
        if (wp == kUiTestTimer) {
            UiTestTick();
            return 0;
        }
        if (wp == kStackFlashTimer) {
            if (--stackFlash_ <= 0) KillTimer(hwnd_, wp);
            regsView_->Call(SCI_INDICSETALPHA, 9, std::max(0, stackFlash_) * 30);
            regsView_->Call(SCI_INDICSETOUTLINEALPHA, 9, std::max(0, stackFlash_) * 40);
            return 0;
        }
        if (wp == kOccurrenceTimer) {
            KillTimer(hwnd_, wp);
            if (Editor* ed = Active()) HighlightOccurrences(ed);
            return 0;
        }
        if (wp == kOutlineTimer) {
            KillTimer(hwnd_, wp);
            RefreshOutline();
            return 0;
        }
        if (wp == kInlayTimer) {
            KillTimer(hwnd_, wp);
            for (Editor* e : {Active(), SplitEditor()}) {
                if (!e) continue;
                UpdateInlayHints(e);
                UpdateChangeMarks(e);
            }
            return 0;
        }
        if (wp == kBackupTimer) {
            WriteBackups();
            return 0;
        }
        if (wp >= kCheckTimerBase) {
            KillTimer(hwnd_, wp);
            if (Editor* ed = FindById(static_cast<int>(wp - kCheckTimerBase))) RequestCheck(ed);
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp) Tip::Hide();
        if (wp) {
            RefreshTree();
            CheckExternalChanges();
            for (auto& e : editors_) RequestBaseline(e.get());  // a commit made meanwhile
        } else if (settings_->autosave) {
            for (auto& e : editors_) {
                if (e->Modified() && !e->file().empty()) Save(e.get());
            }
        }
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
    case WM_APP_OUTPUT:
    case WM_APP_INFO: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        Print(*text, static_cast<Out>(wp));
        return 0;
    }
    case WM_APP_SOLUTION_DONE: {
        std::unique_ptr<SolutionDone> done(reinterpret_cast<SolutionDone*>(lp));
        bool all = done->built && done->passed == done->total;
        Print(done->summary, all ? Out::Success : Out::Error);
        if (!done->id.empty()) RecordResult(done->id, done->built ? done->passed : 0, done->total);
        ShowPane(Pane::Output);
        building_ = false;
        InvalidateChrome();
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
    case WM_DEBUG_EXITED: {
        std::unique_ptr<std::string> stats(reinterpret_cast<std::string*>(lp));
        OnDebugExited(static_cast<DWORD>(wp), stats ? *stats : std::string());
        return 0;
    }
    case WM_DEBUG_LOG: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        PrintTo(Pane::DebugOutput, *text, static_cast<Out>(wp));
        if (static_cast<Out>(wp) == Out::Warning || static_cast<Out>(wp) == Out::Error) Print(*text, static_cast<Out>(wp));
        return 0;
    }
    case WM_APP_UPDATE_INFO:
        OnUpdateInfo(lp);
        return 0;
    case WM_APP_UPDATE_DONE:
        OnUpdateDone(lp);
        return 0;
    case WM_APP_BASELINE: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        if (Editor* ed = FindById(static_cast<int>(wp))) {
            ed->baseline = *text;
            ed->baselineSet = ed->baselineGit = true;
            ed->changesVersion = -1;
            UpdateChangeMarks(ed);
        }
        return 0;
    }
    case WM_DEBUG_PROFILE: {
        std::unique_ptr<std::vector<ProfileLine>> lines(reinterpret_cast<std::vector<ProfileLine>*>(lp));
        OnProfile(std::move(*lines));
        return 0;
    }
    case WM_DEBUG_ODS: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        std::string t = *text;
        if (t.empty() || t.back() != '\n') t += "\n";
        PrintTo(Pane::DebugOutput, t, Out::Normal);
        InvalidateRect(hwnd_, &rcConsoleHeader_, FALSE);  // the tab shows that there is new text
        return 0;
    }
    case WM_APP_CHECK_DONE: {
        std::unique_ptr<CheckDone> done(reinterpret_cast<CheckDone*>(lp));
        Editor* ed = FindById(done->editorId);
        if (ed && ed->version() == done->version) {
            ed->SetDiagnostics(done->diagnostics);
            RefreshProblems();
            InvalidateChrome();
            InvalidateRect(markBar_, nullptr, FALSE);
            InvalidateRect(markBar2_, nullptr, FALSE);
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
        for (auto& e : editors_) RemoveBackup(e.get());
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
        SaveDebugLayout();  // the parts of the debugger open where they are next time
        {
            auto doomed = std::move(editors_);  // destroyed while the list is already empty
            editors_.clear();
        }
        // The parts' views belong to the main window: back into it before the windows of their own close.
        for (int& group : partGroup_) group = kPanelGroup;
        FixGroups();
        if (g_cbtHook) UnhookWindowsHookEx(g_cbtHook);
        g_cbtHook = nullptr;
        Log::Write("AsmEditor closed");
        DestroyWindow(hwnd_);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(uiTestExit_);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---- commands ---------------------------------------------------------------------------------------------------

void MainWindow::OnCommand(int id) {
    Editor* ed = Current();
    if (id >= ID_RECENT && id < ID_RECENT + 100) {
        size_t i = id - ID_RECENT;
        if (i < settings_->recent.size()) OpenPath(settings_->recent[i]);
        return;
    }
    if (id >= ID_THEME && id < ID_THEME + static_cast<int>(Theme::All().size())) {
        ApplyTheme(Theme::All()[id - ID_THEME].id);
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
        ofn.lpstrFilter = English() ? L"Assembly (*.asm;*.inc;*.s;*.nasm;*.mac)\0*.asm;*.inc;*.s;*.nasm;*.mac\0All files\0*.*\0"
                                : L"Ассемблер (*.asm;*.inc;*.s;*.nasm;*.mac)\0*.asm;*.inc;*.s;*.nasm;*.mac\0Все файлы\0*.*\0";
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
        if (auto dir = PickFolder(Tr(L"Открыть папку проекта"))) OpenProject(*dir);
        break;
    case ID_QUICK_OPEN: QuickOpen(); break;
    case ID_SEARCH_EVERYWHERE: SearchEverywhere(); break;
    case ID_RECENT_FILES: RecentFiles(); break;
    case ID_SAVE: Save(ed); break;
    case ID_SAVE_AS: SaveAs(ed); break;
    case ID_SAVE_ALL: SaveAll(); break;
    case ID_CLOSE_TAB: if (ed) CloseEditor(IndexOf(ed)); break;
    case ID_CLOSE_OTHERS: if (ed) CloseOthers(IndexOf(ed)); break;
    case ID_CLOSE_ALL: CloseOthers(-1); break;
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
    case ID_FIND_IN_FILES: FindInFiles(); break;
    case ID_GOTO:
        if (ed) {
            if (auto s = InputBox(hwnd_, Tr(L"Перейти к строке"), Tr(L"Номер строки:"), std::to_wstring(ed->CaretLine()))) {
                int line = _wtoi(s->c_str());
                if (line > 0) ed->GotoLine(line);
            }
        }
        break;
    case ID_COMMENT: if (ed) ed->ToggleComment(); break;
    case ID_COMPLETE: if (ed) Complete(ed, true); break;
    case ID_SNIPPET: if (ed) ShowSnippets(ed); break;
    case ID_SHOW_HINT: if (ed) ShowHint(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_GOTO_DEF: if (ed) GoToDefinition(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_FIND_USAGES: if (ed) FindUsages(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_RENAME: if (ed) Rename(ed, ed->Call(SCI_GETCURRENTPOS)); break;
    case ID_NAV_BACK: Navigate(true); break;
    case ID_NAV_FORWARD: Navigate(false); break;
    case ID_DUPLICATE: if (ed) ed->Call(SCI_SELECTIONDUPLICATE); break;
    case ID_FORMAT: if (ed) FormatCode(ed); break;
    case ID_QUICK_FIX: if (ed) QuickFix(ed); break;
    case ID_ADD_CARET_UP: if (ed) AddCaret(ed, false); break;
    case ID_ADD_CARET_DOWN: if (ed) AddCaret(ed, true); break;
    case ID_SELECT_NEXT:
        if (ed) {
            ed->Call(SCI_TARGETWHOLEDOCUMENT);
            ed->Call(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
            ed->Call(SCI_MULTIPLESELECTADDNEXT);
            ed->Call(SCI_SCROLLCARET);
        }
        break;
    case ID_SELECT_ALL_OCCURRENCES:
        if (ed) {
            ed->Call(SCI_TARGETWHOLEDOCUMENT);
            ed->Call(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE | SCFIND_WHOLEWORD);
            ed->Call(SCI_MULTIPLESELECTADDEACH);
        }
        break;
    case ID_TOGGLE_FOLD:
        if (ed) {
            Sci_Position line = ed->Call(SCI_LINEFROMPOSITION, ed->Call(SCI_GETCURRENTPOS));
            if (!(ed->Call(SCI_GETFOLDLEVEL, line) & SC_FOLDLEVELHEADERFLAG)) line = ed->Call(SCI_GETFOLDPARENT, line);
            if (line >= 0) ed->Call(SCI_TOGGLEFOLD, line);
        }
        break;
    case ID_FOLD_ALL: if (ed) FoldAll(ed, true); break;
    case ID_UNFOLD_ALL: if (ed) FoldAll(ed, false); break;
    case ID_COMMAND_PALETTE: CommandPalette(); break;
    case ID_ZOOM_IN: ApplyFontSize(settings_->fontSize + 1); break;
    case ID_ZOOM_OUT: ApplyFontSize(settings_->fontSize - 1); break;
    case ID_ZOOM_RESET: ApplyFontSize(11); break;
    case ID_NEXT_TAB:
    case ID_PREV_TAB:  // with Ctrl held (Ctrl+Tab): the switcher; else within the tab row of the focused group
        if (ed && (GetKeyState(VK_CONTROL) & 0x8000) && editors_.size() > 1) {
            SwitchTabs(id == ID_NEXT_TAB);
        } else if (ed) {
            std::vector<int> tabs = GroupTabs(ed->group);
            int n = (int)tabs.size();
            int at = (int)(std::find(tabs.begin(), tabs.end(), IndexOf(ed)) - tabs.begin());
            if (n > 1) Activate(tabs[(at + (id == ID_NEXT_TAB ? 1 : n - 1)) % n]);
        }
        break;
    case ID_SPLIT:
    case ID_SPLIT_DOWN:
        if (ed) SplitWith(IndexOf(ed), id == ID_SPLIT_DOWN);
        break;
    case ID_UNSPLIT: Unsplit(ed ? WindowPrimary(ed->group) : 0); break;
    case ID_DETACH_TAB:
        if (ed && IsFloating(ed)) DockEditor(ed);
        else if (ed) DetachEditor(IndexOf(ed), nullptr);
        break;
    case ID_DETACH_DEBUG: ToggleDebugWindow(); break;
    case ID_PIN_TAB: if (ed) TogglePin(IndexOf(ed)); break;
    case ID_OUTLINE: ShowLeftTab(leftTab_ == LeftTab::Outline ? LeftTab::Project : LeftTab::Outline); break;
    case ID_SHOW_OUTPUT: ShowPane(Pane::Output); break;
    case ID_SHOW_TERMINAL:
        ShowPane(Pane::Terminal);
        SetFocus(View(Pane::Terminal).hwnd());
        break;
    case ID_SHOW_PROBLEMS:
        RefreshProblems();
        ShowPane(Pane::Problems);
        break;
    case ID_INLAY_HINTS:
        settings_->inlayHints = !settings_->inlayHints;
        settings_->Save();
        CheckMenuItem(viewMenu_, ID_INLAY_HINTS, settings_->inlayHints ? MF_CHECKED : MF_UNCHECKED);
        for (auto& e : editors_) {
            e->hintsVersion = -1;
            UpdateInlayHints(e.get());
        }
        break;
    case ID_LANGUAGE:
        settings_->language = English() ? L"ru" : L"en";
        SetLanguage(settings_->language);
        settings_->Save();
        Relocalize();
        break;
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
    case ID_RUN_TO_CURSOR: RunToCursor(); break;
    case ID_SET_NEXT: SetNextStatement(); break;
    case ID_STEP_BACK: StepBack(); break;
    case ID_ATTACH: AttachToProcess(); break;
    case ID_PROFILE_RUN: if (!debugging_) StartBuild(After::Profile); break;
    case ID_PROFILE_CLEAR: ClearProfile(); break;
    case ID_PROFILE_SAMPLE: if (!debugging_) StartBuild(After::Sample); break;
    case ID_TOGGLE_BP:
        if (ed) ToggleBreakpoint(ed, ed->CaretLine());
        break;
    case ID_BP_CONDITION:
        if (ed) EditBreakpointCondition(ed, ed->CaretLine());
        break;
    case ID_CLEAR_BPS:
        breakpoints_.clear();
        for (auto& e : editors_) {
            e->SetBreakpoints({});
            if (debugger_ && !e->file().empty()) debugger_->SetBreakpoints(e->file(), {});
        }
        break;
    case ID_ADD_WATCH:
        if (ed) {
            Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
            std::string what = e > s ? std::string(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, s, e - s)), e - s)
                                     : ed->WordAt(ed->Call(SCI_GETCURRENTPOS));
            if (!what.empty() && what.find('\n') == std::string::npos) AddWatch(what);
        }
        break;
    case ID_DATA_BP: {
        std::string word = ed ? ed->WordAt(ed->Call(SCI_GETCURRENTPOS)) : std::string();
        auto text = InputBox(hwnd_, Tr(L"Точка останова на данные"),
                             Tr(L"Остановиться, когда изменится (адрес или имя, размер через запятую: counter, 4):"),
                             Widen(word));
        if (text && !Trim(*text).empty()) AddDataBreakpoint(Narrow(Trim(*text)));
        break;
    }
    case ID_DEBUG_PANEL:
        showDebugPanel_ = !showDebugPanel_;
        CheckMenuItem(viewMenu_, ID_DEBUG_PANEL, showDebugPanel_ ? MF_CHECKED : MF_UNCHECKED);
        Layout();
        break;
    case ID_STOP: StopProgram(); break;
    case ID_CLEAR_CONSOLE:
        ClearPane(pane_);
        if (pane_ == Pane::Problems) {
            buildDiagnostics_.clear();
            RefreshProblems();
        }
        break;
    case ID_RESET_MAIN: SetMainFile({}); break;
    case ID_RUN_CONFIG: EditRunConfig(); break;
    case ID_CHOOSE_TARGET:
    case ID_TARGET: ChooseTarget(); break;
    case ID_PROFILE: ChooseProfile(); break;
    case ID_PROFILE_DEBUG:
    case ID_PROFILE_RELEASE:
        runConfig_.profile = id == ID_PROFILE_DEBUG ? L"Debug" : L"Release";
        SaveProjectSession();
        Layout();
        break;
    case ID_CHECK_SOLUTION: CheckSolution(); break;
    case ID_NEW_EXERCISE: NewExercise(); break;
    case ID_LESSONS: OpenLesson(); break;
    case ID_LOAD_TASKS: LoadTeacherTasks(); break;
    case ID_EXERCISE_HINT: ExerciseHint(); break;
    case ID_GROUP_SUMMARY: GroupSummary(); break;
    case ID_EXPORT_RESULTS: ExportResults(); break;
    case ID_SETTINGS: {
        std::wstring theme = settings_->theme, language = settings_->language;
        if (ShowSettingsDialog(hwnd_, *settings_)) {
            Log::Enable(settings_->log);
            if (settings_->theme != theme) ApplyTheme(settings_->theme);
            if (settings_->language != language) {
                SetLanguage(settings_->language);
                Relocalize();
            }
            ApplyFontSize(settings_->fontSize);
            RefreshToolStatus();
            for (auto& e : editors_) {
                if (settings_->liveCheck) RequestCheck(e.get());
                else e->SetDiagnostics({});
            }
            InvalidateChrome();
        }
        break;
    }
    case ID_KEYS: EditKeys(); break;
    case ID_HELP_PAGE: ShowHelpPage(); break;
    case ID_UPDATE_APP:
        if (pendingRelease_ && MessageBoxW(hwnd_, (std::wstring(Tr(L"Вышла новая версия AsmEditor: ")) + Widen(pendingRelease_->version) +
                                                   L".\n\n" + Tr(L"Скачать и установить её сейчас?")).c_str(),
                                           L"AsmEditor", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            InstallUpdate(*pendingRelease_);
        } else if (!pendingRelease_) {
            CheckForAppUpdate(true);
        }
        break;
    case ID_ASSOCIATE: ToggleAssociation(); break;
    case ID_CHECK_UPDATES: CheckUpdates(); break;
    case ID_OPEN_LOG: {
        if (!Log::Enabled()) {
            settings_->log = true;
            Log::Enable(true);
            settings_->Save();
            Log::Write("log enabled from the Help menu");
        }
        std::wstring file = Log::File().wstring();
        ShellExecuteW(hwnd_, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case ID_ABOUT:
        ShowMessage(hwnd_,
                    std::wstring(L"AsmEditor " ASMEDITOR_VERSION_TEXT_W L"\n\n") + Tr(L"Редактор и отладчик NASM для Windows.") + L"\nScintilla 5.6.5 · NASM " +
                        Toolchain::NasmVersion(*settings_) + L"\n\n" + Tr(L"Настройки: ") + Settings::Dir().wstring(),
                    false);
        break;
    case ID_TREE_NEW_FILE:
    case ID_TREE_NEW_FOLDER: {
        fs::path target = contextItem_ ? TreePath(contextItem_) : projectRoot_;
        fs::path dir = DirExists(target) ? target : target.parent_path();
        bool folder = id == ID_TREE_NEW_FOLDER;
        auto name = InputBox(hwnd_, folder ? Tr(L"Новая папка") : Tr(L"Новый файл"),
                             folder ? Tr(L"Имя папки:") : Tr(L"Имя файла:"), folder ? L"" : L"main.asm");
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
        if (ec) ShowMessage(hwnd_, std::wstring(Tr(L"Не удалось создать: ")) + Widen(ec.message(), CP_ACP));
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
        auto name = InputBox(hwnd_, Tr(L"Переименовать"), Tr(L"Новое имя:"), from.filename().wstring());
        if (!name || Trim(*name).empty() || Trim(*name) == from.filename().wstring()) break;
        fs::path to = from.parent_path() / Trim(*name);
        if (!MoveFileW(from.c_str(), to.c_str())) {
            ShowMessage(hwnd_, Tr(L"Не удалось переименовать."));
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
        std::wstring q = std::wstring(Tr(L"Переместить в корзину: ")) + L"«" + target.filename().wstring() + L"»?";
        if (MessageBoxW(hwnd_, q.c_str(), Tr(L"Удаление"), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
        std::wstring from = target.wstring();
        from.push_back(L'\0');  // double-null terminated list
        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd_;
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
            ShowMessage(hwnd_, Tr(L"Не удалось удалить."));
            break;
        }
        for (int i = (int)editors_.size() - 1; i >= 0; i--) {
            const fs::path& f = editors_[i]->file();
            if (SamePath(f, target) || IsUnder(f, target)) {
                if (i == splitIndex_) splitIndex_ = -1;
                else if (i < splitIndex_) splitIndex_--;
                if (i < active_) active_--;  // a closed shown tab is replaced by Layout (FixGroups)
                std::unique_ptr<Editor> doomed = std::move(editors_[i]);  // destroyed after the list is whole
                editors_.erase(editors_.begin() + i);
                doomed.reset();
                if (active_ >= (int)editors_.size()) active_ = (int)editors_.size() - 1;
            }
        }
        if (SamePath(mainFile_, target) || IsUnder(mainFile_, target)) SetMainFile({});
        Layout();
        if (active_ >= 0) Activate(active_);
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
    if (nm->hwndFrom == outline_) {
        // Clicking an item (or moving to it with the keyboard) shows its line.
        if (nm->code == TVN_SELCHANGEDW || nm->code == NM_DBLCLK) {
            HTREEITEM item = TreeView_GetSelection(outline_);
            TVITEMW tvi{};
            tvi.mask = TVIF_PARAM;
            tvi.hItem = item;
            if (!item || !TreeView_GetItem(outline_, &tvi)) return;
            size_t index = static_cast<size_t>(tvi.lParam);
            Editor* ed = FindById(outlineEditor_);
            if (!ed || index >= outlineLines_.size()) return;
            ed->GotoLine(outlineLines_[index]);
            bool keyboard = nm->code == TVN_SELCHANGEDW && reinterpret_cast<NMTREEVIEWW*>(nm)->action == TVC_BYKEYBOARD;
            if (keyboard) SetFocus(outline_);
        }
        return;
    }

    auto* scn = reinterpret_cast<SCNotification*>(nm);
    // Lines added out of sight change the scroll range without a repaint.
    if (nm->code == SCN_MODIFIED || nm->code == SCN_ZOOM) ScrollBars::Changed(nm->hwndFrom);
    if (nm->code == SCN_ZOOM) {  // the change marks are images a line tall
        if (Editor* zoomed = FindByHwnd(nm->hwndFrom)) {
            zoomed->changesVersion = -1;
            UpdateChangeMarks(zoomed);
        }
    }
    for (auto& view : views_) {
        if (!view || nm->hwndFrom != view->hwnd()) continue;
        if (nm->code == SCN_DOUBLECLICK && view.get() != &View(Pane::Terminal)) ConsoleDoubleClick(*view, scn->position);
        if (nm->code == SCN_UPDATEUI) view->LimitScroll();
        return;
    }
    for (Editor* v : PanelViews()) {
        if (nm->hwndFrom != v->hwnd()) continue;
        if (nm->code == SCN_DOUBLECLICK) PanelDoubleClick(*v, scn->position);
        if (nm->code == SCN_UPDATEUI) v->LimitScroll();
        return;
    }
    Editor* ed = FindByHwnd(nm->hwndFrom);
    if (!ed) return;
    if (nm->code == SCN_UPDATEUI) ed->LimitScroll();
    switch (nm->code) {
    case SCN_STYLENEEDED:
        ed->Lex(scn->position);
        break;
    case SCN_MODIFIED:
        if (scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
            ed->bumpVersion();
            if (ed == usageEditor_) ClearUsageHighlight();
            if (settings_->liveCheck) SetTimer(hwnd_, kCheckTimerBase + ed->id(), 700, nullptr);
            if (ed->id() == outlineEditor_ || leftTab_ == LeftTab::Outline) SetTimer(hwnd_, kOutlineTimer, 800, nullptr);
            SetTimer(hwnd_, kInlayTimer, 600, nullptr);
            if (scn->linesAdded) {
                InvalidateRect(markBar_, nullptr, FALSE);
                InvalidateRect(markBar2_, nullptr, FALSE);
            }
        }
        break;
    case SCN_SAVEPOINTREACHED:
    case SCN_SAVEPOINTLEFT:
        InvalidateTabs();
        if (IsFloating(ed)) UpdateFloatTitles();
        break;
    case SCN_UPDATEUI:
        InvalidateRect(hwnd_, &rcStatus_, FALSE);
        // The hint card goes when the text scrolls or the caret moves without typing (typing keeps the operands
        // reminder of the line).
        if (scn->updated & (SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL)) lastScrollTick_ = GetTickCount();
        if ((scn->updated & (SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL)) ||
            ((scn->updated & SC_UPDATE_SELECTION) && !(scn->updated & SC_UPDATE_CONTENT))) {
            Tip::HideUnlessNew();
        }
        if (scn->updated & (SC_UPDATE_SELECTION | SC_UPDATE_CONTENT)) {
            UpdateBracePairs(ed);
            SetTimer(hwnd_, kOccurrenceTimer, 300, nullptr);
        }
        if (scn->updated & SC_UPDATE_V_SCROLL) InvalidateRect(ed == SplitEditor() ? markBar2_ : markBar_, nullptr, FALSE);
        break;
    case SCN_FOCUSIN:
        // The group of the focused editor is where commands act and new tabs open.
        floatFocus_ = ed->group >= kPanelGroup ? ed->group : 0;
        if (ed->group < kPanelGroup) focusRight_ = ed->group == 1;
        InvalidateTabs();
        InvalidateRect(hwnd_, &rcStatus_, FALSE);
        if (leftTab_ == LeftTab::Outline && ed->id() != outlineEditor_) RefreshOutline();
        break;
    case SCN_CHARADDED: {
        ed->OnCharAdded(scn->ch);
        int ch = scn->ch;
        if (isalnum(ch) || ch == '_' || ch == '.' || ch == '%' || ch == '@' || ch == '?' || ch >= 0x80) Complete(ed, false);
        else if (ed->Call(SCI_AUTOCACTIVE)) ed->Call(SCI_AUTOCCANCEL);
        if (isalnum(ch) || ch == '_') ShowCallArguments(ed);
        if (ch == ' ' || ch == '\t') ShowSignature(ed);
        else if (ch == '\n' || ch == '\r' || ch == ';') Tip::Hide();
        break;
    }
    case SCN_AUTOCCHARDELETED:  // Backspace while the list is open: filter again
        Complete(ed, false);
        break;
    case SCN_AUTOCSELECTION:
        if (scn->listCompletionMethod != SC_AC_COMMAND && scn->text) {
            // A snippet chosen from the completion list ("loop?10"): insert its body instead of the word.
            std::string text = scn->text;
            for (const Snippets::Snippet& s : Snippets::All()) {
                if (text == std::string(s.name) + "…") {
                    ed->Call(SCI_AUTOCCANCEL);
                    Sci_Position caret = ed->Call(SCI_GETCURRENTPOS);
                    ed->Call(SCI_DELETERANGE, scn->position, caret - scn->position);
                    InsertSnippet(ed, s.name);
                    break;
                }
            }
        }
        break;
    case SCN_USERLISTSELECTION:
        if (scn->listType == 1 && scn->text) {
            std::string name = scn->text;
            size_t q = name.find('?');
            if (q != std::string::npos) name.erase(q);
            InsertSnippet(ed, name);
        }
        break;
    case SCN_DWELLSTART:
        if (GetTickCount() - lastScrollTick_ < 600) break;  // still scrolling (or just stopped): no card
        if (scn->position >= 0 && ed->DiagnosticsAt(static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, scn->position)) + 1)) {
            ed->ShowDiagnosticTip(scn->position);
        } else {
            ShowHint(ed, scn->position);
        }
        break;
    case SCN_MARGINCLICK:
        if (scn->margin == 0) ToggleBreakpoint(ed, static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, scn->position)) + 1);
        break;
    case SCN_MARGINRIGHTCLICK:
        if (scn->margin == 0 || scn->margin == 1) {
            BreakpointMenu(ed, static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, scn->position)) + 1);
        }
        break;
    case SCN_DWELLEND:
        Tip::Hide();
        break;
    }
}
