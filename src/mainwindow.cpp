#include "mw_internal.h"
#include "hints.h"
#include "project.h"

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
