#include "mw_internal.h"

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
