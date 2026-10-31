#include "mw_internal.h"

namespace {

// Width of the mark bar beside each editor's scrollbar.
constexpr int kMarkBarWidth = 10;
// Islands: the gap between the panels, the inset of the windows inside them and the corner radius (96-dpi pixels).
constexpr int kIslandGap = 6;
constexpr int kIslandPadding = 4;
constexpr int kIslandRadius = 10;
}

// ---- layout & painting ------------------------------------------------------------------------------------------

void MainWindow::Layout() {
    if (!tree_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    // The panels are rounded islands on the window background, as in the IntelliJ New UI: g is the gap between
    // them (it is also the splitter you drag), p keeps the child windows clear of the rounded corners.
    int tb = S(40), sb = S(26), header = S(32), g = S(kIslandGap), p = S(kIslandPadding), tabsH = S(34),
        inputH = S(36), mark = S(kMarkBarWidth);
    int treeW = std::clamp(S(settings_->treeWidth), S(120), std::max(S(120), W - S(300)));
    int bodyTop = tb, bodyBottom = H - sb;
    int consoleH = std::clamp(S(settings_->consoleHeight), S(90), std::max(S(90), bodyBottom - bodyTop - S(140)));
    islands_.clear();

    rcToolbar_ = {0, 0, W, tb};
    rcStatus_ = {0, bodyBottom, W, H};
    RECT treeIsland{g, bodyTop, g + treeW, bodyBottom};
    islands_.push_back(treeIsland);
    rcTreeHeader_ = {treeIsland.left, bodyTop, treeIsland.right, bodyTop + header};
    rcRefresh_ = leftTab_ == LeftTab::Project
                     ? RECT{treeIsland.right - S(32), bodyTop + S(4), treeIsland.right - S(8), bodyTop + header - S(4)}
                     : RECT{};
    rcSplitV_ = {treeIsland.right, bodyTop, treeIsland.right + g, bodyBottom};
    int x0 = rcSplitV_.right;

    // Debug panel on the right: tabs (registers, call stack, watches, disassembly) above the memory view.
    bool panel = DebugPanelVisible();
    int debugW = panel ? std::clamp(S(settings_->debugWidth), S(220), std::max(S(220), W - x0 - S(300))) : 0;
    int x1 = panel ? W - g - debugW - g : W - g;  // right edge of the editor column
    rcSplitD_ = panel ? RECT{x1, bodyTop, x1 + g, bodyBottom} : RECT{};
    rcDebug_ = panel ? RECT{x1 + g, bodyTop, W - g, bodyBottom} : RECT{};
    int regsBottom = rcDebug_.top + header + (bodyBottom - bodyTop - header) * 58 / 100;
    rcDebugHeader_ = {rcDebug_.left, bodyTop, rcDebug_.right, bodyTop + header};
    rcWatchRow_ = {rcDebug_.left, rcDebugHeader_.bottom, rcDebug_.right, rcDebugHeader_.bottom + inputH};
    rcMemHeader_ = {rcDebug_.left, regsBottom + g, rcDebug_.right, regsBottom + g + header};
    rcMemFormat_ = {rcDebug_.right - S(110), rcMemHeader_.top + S(4), rcDebug_.right - S(8), rcMemHeader_.bottom - S(4)};
    rcMemRow_ = {rcDebug_.left, rcMemHeader_.bottom, rcDebug_.right, rcMemHeader_.bottom + inputH};
    if (panel) {
        islands_.push_back({rcDebug_.left, bodyTop, rcDebug_.right, regsBottom});
        islands_.push_back({rcDebug_.left, rcMemHeader_.top, rcDebug_.right, bodyBottom});
    }

    rcSplitH_ = {x0, bodyBottom - consoleH - g, x1, bodyBottom - consoleH};
    islands_.push_back({x0, bodyTop, x1, rcSplitH_.top});
    islands_.push_back({x0, rcSplitH_.bottom, x1, bodyBottom});
    rcTabs_ = {x0 + p, bodyTop, x1 - p, bodyTop + tabsH};
    rcEditor_ = {x0 + p, bodyTop + tabsH, x1 - p, rcSplitH_.top - p};
    Editor* right = SplitEditor();
    if (right) {  // two editors side by side
        int split = S(5), mid = (rcEditor_.left + rcEditor_.right) / 2;
        rcEditor2_ = {mid + split / 2, rcEditor_.top, rcEditor_.right, rcEditor_.bottom};
        rcSplitE_ = {mid - split / 2, rcEditor_.top, mid + split / 2, rcEditor_.bottom};
        rcEditor_.right = rcSplitE_.left;
    } else {
        rcEditor2_ = rcSplitE_ = {};
    }
    rcConsoleHeader_ = {x0, rcSplitH_.bottom, x1, rcSplitH_.bottom + header};
    rcClear_ = {x1 - S(90), rcConsoleHeader_.top, x1 - S(12), rcConsoleHeader_.bottom};

    bool watchTab = panel && debugTab_ == DebugTab::Watch;
    for (HWND h : {memView_->hwnd(), memExpr_}) ShowWindow(h, panel ? SW_SHOW : SW_HIDE);
    ShowWindow(watchExpr_, watchTab ? SW_SHOW : SW_HIDE);
    Editor* tabViews[] = {regsView_.get(), stackView_.get(), watchView_.get(), disasmView_.get(), flagsView_.get()};
    for (int i = 0; i < static_cast<int>(DebugTab::Count); i++) {
        ShowWindow(tabViews[i]->hwnd(), panel && i == static_cast<int>(debugTab_) ? SW_SHOW : SW_HIDE);
    }
    if (panel) {
        int top = watchTab ? rcWatchRow_.bottom : rcDebugHeader_.bottom;
        SetWindowPos(tabViews[static_cast<int>(debugTab_)]->hwnd(), nullptr, rcDebug_.left + p, top, debugW - 2 * p,
                     regsBottom - p - top, SWP_NOZORDER);
        // The inputs sit inside a rounded frame that Paint draws around them.
        if (watchTab) {
            SetWindowPos(watchExpr_, nullptr, rcDebug_.left + S(16), rcWatchRow_.top + S(7), debugW - S(32),
                         inputH - S(14), SWP_NOZORDER);
        }
        SetWindowPos(memExpr_, nullptr, rcDebug_.left + S(16), rcMemRow_.top + S(7), debugW - S(32), inputH - S(14),
                     SWP_NOZORDER);
        SetWindowPos(memView_->hwnd(), nullptr, rcDebug_.left + p, rcMemRow_.bottom, debugW - 2 * p,
                     bodyBottom - p - rcMemRow_.bottom, SWP_NOZORDER);
    }

    // Left panel: project tree or file outline.
    ShowWindow(tree_, leftTab_ == LeftTab::Project ? SW_SHOW : SW_HIDE);
    ShowWindow(outline_, leftTab_ == LeftTab::Outline ? SW_SHOW : SW_HIDE);
    HDWP dwp = BeginDeferWindowPos(8 + (int)editors_.size());
    dwp = DeferWindowPos(dwp, leftTab_ == LeftTab::Project ? tree_ : outline_, nullptr, treeIsland.left + p,
                         rcTreeHeader_.bottom, treeW - 2 * p, bodyBottom - p - rcTreeHeader_.bottom, SWP_NOZORDER);
    for (auto& view : views_) {
        dwp = DeferWindowPos(dwp, view->hwnd(), nullptr, x0 + p, rcConsoleHeader_.bottom, x1 - x0 - 2 * p,
                             bodyBottom - p - rcConsoleHeader_.bottom, SWP_NOZORDER);
    }
    Editor* left = Active();
    for (auto& ed : editors_) {
        const RECT& r = ed.get() == right ? rcEditor2_ : rcEditor_;
        dwp = DeferWindowPos(dwp, ed->hwnd(), nullptr, r.left, r.top, r.right - r.left - mark, r.bottom - r.top,
                             SWP_NOZORDER);
    }
    dwp = DeferWindowPos(dwp, markBar_, nullptr, rcEditor_.right - mark, rcEditor_.top, mark,
                         rcEditor_.bottom - rcEditor_.top, SWP_NOZORDER | (left ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    dwp = DeferWindowPos(dwp, markBar2_, nullptr, rcEditor2_.right - mark, rcEditor2_.top, mark,
                         rcEditor2_.bottom - rcEditor2_.top, SWP_NOZORDER | (right ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    EndDeferWindowPos(dwp);
    for (size_t i = 0; i < editors_.size(); i++) {
        Editor* e = editors_[i].get();
        ShowWindow(e->hwnd(), e == left || e == right ? SW_SHOW : SW_HIDE);
    }

    if (termActive_ && pty_) {  // keep the pseudoconsole the size of the terminal view
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
    // In a narrow window the buttons with icons lose their labels, so nothing overlaps.
    auto width = [&](const ToolItem& t, bool compact) {
        if (compact && t.icon && !t.menu) return S(32);
        if (t.id == ID_PROJECT_WIDGET) return TextWidth(dc, ToolLabel(t)) + S(44);
        return TextWidth(dc, ToolLabel(t)) + S(t.icon || t.id == ID_TARGET ? 36 : 20);
    };
    auto visible = [&](const ToolItem& t) {
        if (t.id == ID_PROJECT_WIDGET && projectRoot_.empty()) return false;
        return !((t.show == 1 && !debugging_) || (t.show == 2 && debugging_));
    };
    int needed = S(60);
    for (const ToolItem& t : tools_) {
        if (visible(t)) needed += width(t, false) + S(4);
    }
    compactTools_ = needed > rc.right;
    int x = S(6), rightEdge = rc.right - S(6);
    for (size_t i = 0; i < tools_.size(); i++) {
        ToolItem& t = tools_[i];
        if (!visible(t)) {
            t.rc = {};
            continue;
        }
        int w = width(t, compactTools_);
        if (t.right) {
            t.rc = {rightEdge - w, S(6), rightEdge, tb - S(6)};
            rightEdge -= w + S(4);
        } else {
            if (i > 0 && tools_[i - 1].menu && !t.menu) x += S(14);  // gap after the menus
            if (t.id == ID_TARGET) x += S(14);                        // and before the run target
            if (t.id == ID_BUILD && tools_[i - 1].rc.right > 0) x += S(10);  // and after the project widget
            t.rc = {x, S(6), x + w, tb - S(6)};
            x += w + S(2);
        }
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRect(markBar_, nullptr, FALSE);
    InvalidateRect(markBar2_, nullptr, FALSE);
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

std::wstring MainWindow::ToolLabel(const ToolItem& t) const {
    if (t.id == ID_TARGET) {  // what runs: the main file or the active tab
        fs::path target = !mainFile_.empty() ? mainFile_ : (Active() ? Active()->file() : fs::path());
        std::wstring text = target.empty() ? Tr(L"активный файл") : target.filename().wstring();
        if (runConfig_.Wsl()) text += L" · Linux";
        if (runConfig_.output != L"exe") text += L" · " + runConfig_.output;
        return text + L"  ▾";
    }
    if (t.id == ID_PROFILE) return runConfig_.profile + L"  ▾";
    if (t.id == ID_PROJECT_WIDGET) return projectRoot_.filename().wstring() + L"  ▾";
    return Tr(t.text);
}

// A filled rounded rectangle; outline = CLR_INVALID draws no border.
static void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF outline = CLR_INVALID) {
    HPEN pen = outline == CLR_INVALID ? nullptr : CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ oldPen = SelectObject(dc, pen ? pen : GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, fill);
    // Without a pen RoundRect leaves out the right and bottom edge.
    int extra = pen ? 0 : 1;
    RoundRect(dc, r.left, r.top, r.right + extra, r.bottom + extra, 2 * radius, 2 * radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (pen) DeleteObject(pen);
}

// Tabs drawn in a panel header ("Проект | Структура" and so on): the active one is bright and underlined.
void MainWindow::PaintHeaderTabs(HDC dc, const RECT& area, const std::vector<std::wstring>& titles, int active,
                                 std::vector<RECT>& rects, int hover) {
    rects.assign(titles.size(), RECT{});
    // When the titles do not fit, every tab gets an equal share and its title is shortened with "…".
    int total = 0;
    SelectObject(dc, uiFont_);
    for (const std::wstring& t : titles) total += TextWidth(dc, t) + S(20);
    int available = area.right - area.left - S(14);
    int cap = total > available && !titles.empty() ? std::max(S(24), available / static_cast<int>(titles.size()) - S(20)) : INT_MAX;
    int x = area.left + S(14);
    for (size_t i = 0; i < titles.size(); i++) {
        bool on = static_cast<int>(i) == active;
        int w = std::min(TextWidth(dc, titles[i]), cap);
        RECT r{x - S(6), area.top + S(4), x + w + S(6), area.bottom - S(4)};
        if (r.right > area.right) break;
        rects[i] = r;
        if (!on && static_cast<int>(i) == hover) FillRound(dc, r, S(6), Theme::Hover);
        DrawTextColor(dc, titles[i], {x, area.top, x + w + S(2), area.bottom},
                      on ? Theme::Bright : (static_cast<int>(i) == hover ? Theme::Text : Theme::Muted),
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (on) FillRound(dc, {x - S(2), area.bottom - S(3), x + w + S(2), area.bottom - S(1)}, S(1), Theme::Accent);
        x += w + S(20);
    }
}

void MainWindow::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    FillRectColor(dc, rc, Theme::FrameBg);
    for (const RECT& island : islands_) FillRound(dc, island, S(kIslandRadius), Theme::EditorBg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, uiFont_);

    PaintToolbar(dc);

    // left panel header
    PaintHeaderTabs(dc, rcTreeHeader_, {Tr(L"Проект"), Tr(L"Структура")}, static_cast<int>(leftTab_), leftTabRects_,
                    hoverLeftTab_);
    if (leftTab_ == LeftTab::Project) {
        if (!projectRoot_.empty()) {
            if (hoverRefresh_) FillRound(dc, rcRefresh_, S(6), Theme::Hover);
            DrawIcon(dc, 5, rcRefresh_, true);
        } else {
            RECT hint{rcTreeHeader_.left + S(14), rcTreeHeader_.bottom + S(10), rcTreeHeader_.right - S(8),
                      rcTreeHeader_.bottom + S(90)};
            DrawTextColor(dc, Tr(L"Проект не открыт.\nCtrl+Shift+O — открыть папку."), hint, Theme::Dim, DT_WORDBREAK);
        }
    }

    // The splitters are the gaps between the islands; only the split editor has a line of its own.
    if (SplitEditor()) {
        int mid = (rcSplitE_.left + rcSplitE_.right) / 2;
        FillRectColor(dc, {mid, rcSplitE_.top + S(4), mid + 1, rcSplitE_.bottom - S(4)}, Theme::Border);
    }

    PaintTabs(dc);
    if (editors_.empty()) PaintWelcome(dc);

    if (DebugPanelVisible()) {
        RECT tabs = rcDebugHeader_;
        tabs.right -= S(4);
        PaintHeaderTabs(dc, tabs, {Tr(L"Регистры"), Tr(L"Стек вызовов"), Tr(L"Наблюдение"), Tr(L"Код"), Tr(L"Флаги")},
                        static_cast<int>(debugTab_), debugTabRects_, hoverDebugTab_);
        RECT m = rcMemHeader_;
        m.left += S(14);
        DrawTextColor(dc, Tr(L"Память"), m, Theme::Bright, DT_SINGLELINE | DT_VCENTER);
        static const wchar_t* formats[] = {L"байты", L"word", L"dword", L"qword", L"float", L"double", L"текст"};
        std::wstring format = std::wstring(Tr(formats[static_cast<int>(memFormat_)])) + L"  ▾";
        if (hoverMemFormat_) FillRound(dc, rcMemFormat_, S(6), Theme::Hover);
        DrawTextColor(dc, format, rcMemFormat_, hoverMemFormat_ ? Theme::Bright : Theme::Text,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        if (!debugStatus_.empty()) {  // stop reason under the tabs, right-aligned in the memory header
            RECT st = rcMemHeader_;
            st.left += S(80);
            st.right = rcMemFormat_.left - S(8);
            DrawTextColor(dc, debugStatus_, st, debugStatusColor_, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
        }
        for (HWND input : {memExpr_, debugTab_ == DebugTab::Watch ? watchExpr_ : nullptr}) {
            if (!input) continue;
            RECT frame;
            GetWindowRect(input, &frame);
            MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&frame), 2);
            InflateRect(&frame, S(6), S(4));
            FillRound(dc, frame, S(6), Theme::InputBg, GetFocus() == input ? Theme::Accent : Theme::Border);
        }
    }

    // bottom panel: tabs and the state of the program
    int problems = problemCount_;
    std::wstring problemsTitle = Tr(L"Проблемы");
    if (problems) problemsTitle += L" (" + std::to_wstring(problems) + L")";
    RECT tabsArea = rcConsoleHeader_;
    tabsArea.right = rcClear_.left;
    PaintHeaderTabs(dc, tabsArea, {Tr(L"Вывод"), Tr(L"Терминал"), problemsTitle, Tr(L"Поиск"), Tr(L"Отладочный вывод")},
                    static_cast<int>(pane_), paneRects_, hoverPane_);
    RECT state = tabsArea;  // after the last tab that fitted; nothing when there is no room left
    for (const RECT& r : paneRects_) state.left = std::max(state.left, r.right + S(16));
    if (state.right - state.left < S(60)) {
        // no room for the state text
    } else if (debugging_) {
        bool stopped = DebugStopped();
        DrawTextColor(dc, stopped ? Tr(L"● отладка: остановлено") : Tr(L"● отладка: выполняется"), state,
                      stopped ? Theme::Warning : Theme::Success, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else if (process_) {
        DrawTextColor(dc, Tr(L"● программа выполняется — ввод во вкладке «Терминал»"), state, Theme::Success,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else if (building_) {
        DrawTextColor(dc, Tr(L"● сборка…"), state, Theme::Warning, DT_SINGLELINE | DT_VCENTER);
    }
    DrawTextColor(dc, Tr(L"Очистить"), rcClear_, hoverClear_ ? Theme::Bright : Theme::Muted,
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
        return Theme::Blend(c, Theme::FrameBg, 67);
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
        SetDCBrushColor(dc, Theme::FrameBg);
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
        case ID_TARGET:
        case ID_PROFILE:
        case ID_PROJECT_WIDGET: enabled = !debugging_ && !building_; break;
        }
        if ((int)i == hoverTool_ && enabled) FillRound(dc, t.rc, S(6), Theme::Hover);
        RECT tr = t.rc;
        std::wstring text = ToolLabel(t);
        if (t.id == ID_PROJECT_WIDGET) {  // a colored square with the project's initials, its name and ▾
            std::wstring name = projectRoot_.filename().wstring();
            static const uint32_t colors[] = {0x3574F0, 0x5FB865, 0xE0943A, 0xC77DBB, 0x2AACB8, 0xDB5C5C, 0x8F7EE7};
            size_t hash = std::hash<std::wstring>{}(name);
            int cy = (tr.top + tr.bottom) / 2;
            RECT square{tr.left + S(6), cy - S(10), tr.left + S(26), cy + S(10)};
            FillRound(dc, square, S(5), Hex(colors[hash % std::size(colors)]));
            std::wstring initials;
            for (size_t k = 0; k < name.size() && initials.size() < 2; k++) {
                bool start = k == 0 || name[k - 1] == L' ' || name[k - 1] == L'-' || name[k - 1] == L'_' || name[k - 1] == L'.';
                if (start && iswalnum(name[k])) initials += static_cast<wchar_t>(towupper(name[k]));
            }
            if (initials.size() < 2 && name.size() > 1) initials = {static_cast<wchar_t>(towupper(name[0])), static_cast<wchar_t>(towupper(name[1]))};
            SelectObject(dc, uiBold_);
            DrawTextColor(dc, initials, square, RGB(255, 255, 255), DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            SelectObject(dc, uiFont_);
            tr.left = square.right + S(8);
            DrawTextColor(dc, text, tr, enabled ? Theme::Bright : Theme::Dim, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            continue;
        }
        if (t.id == ID_TARGET || t.id == ID_PROFILE) {  // drop-downs: what runs and the build profile
            if (t.id == ID_TARGET) {
                RECT ir{tr.left + S(6), tr.top, tr.left + S(26), tr.bottom};
                DrawIcon(dc, 2, ir, enabled);
                tr.left += S(28);
                DrawTextColor(dc, text, tr, enabled ? Theme::Text : Theme::Dim, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            } else {
                DrawTextColor(dc, text, tr, enabled ? Theme::Text : Theme::Dim, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
            continue;
        }
        if (t.icon) {
            RECT ir{tr.left + S(6), tr.top, tr.left + S(26), tr.bottom};
            DrawIcon(dc, t.icon, ir, enabled);
            tr.left += S(28);
            if (!compactTools_) DrawTextColor(dc, text, tr, enabled ? Theme::Text : Theme::Dim, DT_SINGLELINE | DT_VCENTER);
        } else {
            DrawTextColor(dc, text, tr, Theme::Text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        if (i + 1 < tools_.size() && t.menu && !tools_[i + 1].menu) {
            int sx = t.rc.right + S(8);
            FillRectColor(dc, {sx, t.rc.top + S(4), sx + 1, t.rc.bottom - S(4)}, Theme::Border);
        }
    }
}

void MainWindow::PaintTabs(HDC dc) {
    tabRects_.assign(editors_.size(), RECT{});
    closeRects_.assign(editors_.size(), RECT{});
    if (editors_.empty()) return;

    std::vector<int> widths;
    int total = 0;
    SelectObject(dc, uiFont_);
    for (auto& ed : editors_) {
        std::wstring title = (ed->Modified() ? L"● " : L"") + ed->Title();
        int w = TextWidth(dc, title) + S(ed->pinned ? 62 : 50);
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
        bool inRight = (int)i == splitIndex_;
        bool focused = active ? !(focusRight_ && SplitEditor()) : inRight && focusRight_;
        // IntelliJ-style tabs: no boxes, the open tab is underlined (in the accent color while it has the focus),
        // the one under the mouse gets a rounded highlight.
        bool open = active || inRight;
        RECT pill{r.left + S(2), r.top + S(5), r.right - S(2), r.bottom - S(5)};
        if ((int)i == dragTab_ && drag_ == Drag::Tab && dragMoved_) FillRound(dc, pill, S(6), Theme::Hover, Theme::Accent);
        else if (!open && (int)i == hoverTab_) FillRound(dc, pill, S(6), Theme::Hover);
        if (open) {
            FillRound(dc, {r.left + S(6), r.bottom - S(3), r.right - S(6), r.bottom}, S(1),
                      focused ? Theme::Accent : Theme::Dim);
        }

        bool errors = ed.Count(Severity::Error) > 0;
        bool isMain = !mainFile_.empty() && SamePath(ed.file(), mainFile_);
        std::wstring title = (ed.Modified() ? L"● " : L"") + ed.Title();
        int textLeft = r.left + S(12);
        if (ed.pinned) {  // pinned: a small accent mark before the title
            SetDCBrushColor(dc, Theme::Accent);
            SelectObject(dc, GetStockObject(DC_BRUSH));
            SelectObject(dc, GetStockObject(NULL_PEN));
            int cy = (r.top + r.bottom) / 2;
            Ellipse(dc, textLeft, cy - S(3), textLeft + S(7), cy + S(4));
            textLeft += S(12);
        }
        RECT tr{textLeft, r.top, r.right - S(28), r.bottom};
        SelectObject(dc, isMain ? uiBold_ : uiFont_);
        DrawTextColor(dc, title, tr, errors ? Theme::Error : (open ? Theme::Bright : Theme::Text),
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        int cy = (r.top + r.bottom) / 2;
        RECT cr{r.right - S(28), cy - S(9), r.right - S(10), cy + S(9)};
        closeRects_[i] = cr;
        if ((int)i == hoverClose_) FillRound(dc, cr, S(4), Theme::Pressed);
        if (open || (int)i == hoverTab_) {
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
    DrawTextColor(dc, Tr(L"Редактор NASM: подсветка, проверка ошибок, сборка, запуск и отладка"),
                  {x, y, rcEditor_.right, y + S(20)}, Theme::Muted, DT_SINGLELINE);
    y += S(40);
    const std::pair<int, const wchar_t*> hints[] = {
        {ID_NEW, L"новый файл"},
        {ID_OPEN_FILE, L"открыть файл"},
        {ID_OPEN_FOLDER, L"открыть папку проекта"},
        {ID_QUICK_OPEN, L"перейти к файлу проекта"},
        {ID_COMMAND_PALETTE, L"все команды"},
        {ID_RUN, L"собрать и запустить"},
        {ID_DEBUG, L"отладка (F9 — точка останова, F10/F11 — шаги)"},
        {ID_SETTINGS, L"настройки NASM и линкера"},
    };
    for (auto [id, text] : hints) {
        std::wstring keys = KeysFor(id);
        size_t slash = keys.find(L" / ");
        if (slash != std::wstring::npos) keys.erase(slash);
        DrawTextColor(dc, keys, {x, y, x + S(130), y + S(20)}, Theme::Link, DT_SINGLELINE);
        DrawTextColor(dc, Tr(text), {x + S(140), y, rcEditor_.right, y + S(20)}, Theme::Text, DT_SINGLELINE);
        y += S(24);
    }
    if (!settings_->recent.empty()) {
        y += S(20);
        SelectObject(dc, uiBold_);
        DrawTextColor(dc, Tr(L"Недавние проекты"), {x, y, rcEditor_.right, y + S(20)}, Theme::Muted, DT_SINGLELINE);
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
    FillRectColor(dc, rcStatus_, Theme::FrameBg);
    SelectObject(dc, uiFont_);
    RECT left = rcStatus_;
    left.left += S(14);
    DrawTextColor(dc, toolStatus_, left, toolStatusWarn_ ? Theme::Warning : Theme::Muted, DT_SINGLELINE | DT_VCENTER);

    std::vector<std::pair<std::wstring, COLORREF>> parts;
    Editor* ed = focusRight_ && SplitEditor() ? SplitEditor() : Active();
    if (ed) {
        int errors = ed->Count(Severity::Error), warnings = ed->Count(Severity::Warning);
        if (errors) parts.push_back({std::wstring(Tr(L"Ошибок: ")) + std::to_wstring(errors), Theme::Error});
        if (warnings) parts.push_back({std::wstring(Tr(L"Предупреждений: ")) + std::to_wstring(warnings), Theme::Warning});
        int carets = static_cast<int>(ed->Call(SCI_GETSELECTIONS));
        if (carets > 1) parts.push_back({std::wstring(Tr(L"Курсоров: ")) + std::to_wstring(carets), Theme::Link});
        parts.push_back({std::wstring(Tr(L"Стр ")) + std::to_wstring(ed->CaretLine()) + Tr(L", Кол ") +
                             std::to_wstring(ed->CaretColumn()), Theme::Muted});
        parts.push_back({ed->EncodingName(), Theme::Muted});
        parts.push_back({ed->EolName(), Theme::Muted});
    }
    int x = rcStatus_.right - S(14);
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        int w = TextWidth(dc, it->first);
        DrawTextColor(dc, it->first, {x - w, rcStatus_.top, x, rcStatus_.bottom}, it->second, DT_SINGLELINE | DT_VCENTER);
        x -= w + S(20);
    }
}

void MainWindow::OnMouseMove(int x, int y) {
    if (drag_ == Drag::Tree) {
        settings_->treeWidth = std::max(80, MulDiv(x - S(kIslandGap), 96, dpi_));
        Layout();
        return;
    }
    if (drag_ == Drag::Console) {
        settings_->consoleHeight = std::max(60, MulDiv(rcStatus_.top - y, 96, dpi_));
        Layout();
        return;
    }
    if (drag_ == Drag::Debug) {
        settings_->debugWidth = std::max(180, MulDiv(rcStatus_.right - S(2 * kIslandGap) - x, 96, dpi_));
        Layout();
        return;
    }
    if (drag_ == Drag::Tab) {  // reorder: the dragged tab takes the place of the tab under the mouse
        if (!dragMoved_ && std::abs(x - dragStartX_) < S(8)) return;
        dragMoved_ = true;
        for (size_t i = 0; i < tabRects_.size(); i++) {
            if (static_cast<int>(i) == dragTab_ || x < tabRects_[i].left || x >= tabRects_[i].right) continue;
            if (editors_[i]->pinned != editors_[dragTab_]->pinned) break;  // pinned tabs stay in front
            MoveTab(dragTab_, static_cast<int>(i));
            dragTab_ = static_cast<int>(i);
            break;
        }
        InvalidateRect(hwnd_, &rcTabs_, FALSE);
        return;
    }
    int tool = -1, tab = -1, close = -1, link = -1, pane = -1, leftTab = -1, debugTab = -1;
    for (size_t i = 0; i < tools_.size(); i++) if (Contains(tools_[i].rc, x, y)) tool = (int)i;
    for (size_t i = 0; i < tabRects_.size(); i++) {
        if (Contains(tabRects_[i], x, y) && Contains(rcTabs_, x, y)) tab = (int)i;
        if (Contains(closeRects_[i], x, y) && Contains(rcTabs_, x, y)) close = (int)i;
    }
    if (editors_.empty()) {
        for (size_t i = 0; i < welcomeLinks_.size(); i++) if (Contains(welcomeLinks_[i].first, x, y)) link = (int)i;
    }
    for (size_t i = 0; i < paneRects_.size(); i++) if (Contains(paneRects_[i], x, y)) pane = (int)i;
    for (size_t i = 0; i < leftTabRects_.size(); i++) if (Contains(leftTabRects_[i], x, y)) leftTab = (int)i;
    if (DebugPanelVisible()) {
        for (size_t i = 0; i < debugTabRects_.size(); i++) if (Contains(debugTabRects_[i], x, y)) debugTab = (int)i;
    }
    bool clear = Contains(rcClear_, x, y);
    bool refresh = !projectRoot_.empty() && Contains(rcRefresh_, x, y);
    bool memFormat = DebugPanelVisible() && Contains(rcMemFormat_, x, y);
    if (tool != hoverTool_ || tab != hoverTab_ || close != hoverClose_ || link != hoverLink_ || clear != hoverClear_ ||
        refresh != hoverRefresh_ || pane != hoverPane_ || leftTab != hoverLeftTab_ || debugTab != hoverDebugTab_ ||
        memFormat != hoverMemFormat_) {
        hoverTool_ = tool;
        hoverTab_ = tab;
        hoverClose_ = close;
        hoverLink_ = link;
        hoverClear_ = clear;
        hoverRefresh_ = refresh;
        hoverPane_ = pane;
        hoverLeftTab_ = leftTab;
        hoverDebugTab_ = debugTab;
        hoverMemFormat_ = memFormat;
        InvalidateChrome();
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
    TrackMouseEvent(&tme);
}

void MainWindow::OnClick(int x, int y, bool middle) {
    for (size_t i = 0; i < tabRects_.size(); i++) {
        if (!Contains(tabRects_[i], x, y) || !Contains(rcTabs_, x, y)) continue;
        if ((middle && !editors_[i]->pinned) || Contains(closeRects_[i], x, y)) CloseEditor((int)i);
        else if (!middle) Activate((int)i);
        return;
    }
    if (middle) return;
    for (const ToolItem& t : tools_) {
        if (!Contains(t.rc, x, y)) continue;
        if (t.menu) {
            HMENU menu = t.id == ID_MENU_FILE ? fileMenu_ : t.id == ID_MENU_EDIT ? editMenu_
                       : t.id == ID_MENU_VIEW ? viewMenu_ : t.id == ID_MENU_HELP ? helpMenu_ : runMenu_;
            if (menu == fileMenu_) {
                while (GetMenuItemCount(recentMenu_) > 0) DeleteMenu(recentMenu_, 0, MF_BYPOSITION);
                for (size_t i = 0; i < settings_->recent.size(); i++) {
                    AppendMenuW(recentMenu_, MF_STRING, ID_RECENT + i, settings_->recent[i].c_str());
                }
                if (settings_->recent.empty()) AppendMenuW(recentMenu_, MF_STRING | MF_GRAYED, 0, Tr(L"(пусто)"));
            }
            EnableMenuItem(runMenu_, ID_STOP, process_ || debugging_ ? MF_ENABLED : MF_GRAYED);
            CheckMenuItem(runMenu_, ID_PROFILE_DEBUG, !runConfig_.Release() ? MF_CHECKED : MF_UNCHECKED);
            CheckMenuItem(runMenu_, ID_PROFILE_RELEASE, runConfig_.Release() ? MF_CHECKED : MF_UNCHECKED);
            for (int id : {ID_STEP_OVER, ID_STEP_INTO, ID_STEP_OUT, ID_SET_NEXT}) {
                EnableMenuItem(runMenu_, id, !debugging_ || DebugStopped() ? MF_ENABLED : MF_GRAYED);
            }
            POINT p{t.rc.left, t.rc.bottom};
            ClientToScreen(hwnd_, &p);
            TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, hwnd_, nullptr);
        } else if (t.id == ID_PROJECT_WIDGET) {
            ProjectMenu(t.rc);
        } else {
            OnCommand(t.id);
        }
        return;
    }
    for (size_t i = 0; i < paneRects_.size(); i++) {
        if (!Contains(paneRects_[i], x, y)) continue;
        Pane p = static_cast<Pane>(i);
        if (p == Pane::Problems) RefreshProblems();
        ShowPane(p);
        if (p == Pane::Terminal) SetFocus(View(p).hwnd());
        return;
    }
    for (size_t i = 0; i < leftTabRects_.size(); i++) {
        if (Contains(leftTabRects_[i], x, y)) {
            ShowLeftTab(static_cast<LeftTab>(i));
            return;
        }
    }
    if (DebugPanelVisible()) {
        for (size_t i = 0; i < debugTabRects_.size(); i++) {
            if (Contains(debugTabRects_[i], x, y)) {
                ShowDebugTab(static_cast<DebugTab>(i));
                return;
            }
        }
        if (Contains(rcMemFormat_, x, y)) {
            HMENU menu = CreatePopupMenu();
            static const wchar_t* formats[] = {L"байты", L"word (2 байта)", L"dword (4 байта)", L"qword (8 байт)",
                                               L"float", L"double", L"текст"};
            for (int i = 0; i < static_cast<int>(MemFormat::Count); i++) {
                AppendMenuW(menu, MF_STRING | (i == static_cast<int>(memFormat_) ? MF_CHECKED : 0), i + 1, Tr(formats[i]));
            }
            POINT p{rcMemFormat_.left, rcMemFormat_.bottom};
            ClientToScreen(hwnd_, &p);
            int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, hwnd_, nullptr);
            DestroyMenu(menu);
            if (cmd > 0) {
                memFormat_ = static_cast<MemFormat>(cmd - 1);
                prevMem_.clear();
                RenderMemory();
                InvalidateRect(hwnd_, &rcMemHeader_, FALSE);
            }
            return;
        }
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

// The project widget's menu: the recent projects (the open one checked) and "Open folder".
void MainWindow::ProjectMenu(const RECT& anchor) {
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < settings_->recent.size(); i++) {
        bool current = SamePath(fs::path(settings_->recent[i]), projectRoot_);
        AppendMenuW(menu, MF_STRING | (current ? MF_CHECKED : 0), ID_RECENT + i, settings_->recent[i].c_str());
    }
    if (!settings_->recent.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendCommand(menu, ID_OPEN_FOLDER);
    POINT p{anchor.left, anchor.bottom};
    ClientToScreen(hwnd_, &p);
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::OnRightClick(int x, int y) {
    for (size_t i = 0; i < tabRects_.size(); i++) {
        if (Contains(tabRects_[i], x, y) && Contains(rcTabs_, x, y)) {
            TabMenu(static_cast<int>(i));
            return;
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

// ---- mark bar ---------------------------------------------------------------------------------------------------

LRESULT CALLBACK MainWindow::MarkBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) self->PaintMarkBar(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (self) self->MarkBarClick(hwnd, GET_Y_LPARAM(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Marks along the editor, at the height of their line in the whole file: errors, warnings, breakpoints, the
// execution line and highlighted search hits; a light band shows the visible part.
void MainWindow::PaintMarkBar(HWND bar, HDC dc) {
    RECT rc;
    GetClientRect(bar, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ old = SelectObject(mem, bmp);
    FillRectColor(mem, rc, Theme::EditorBg);
    Editor* ed = bar == markBar2_ ? SplitEditor() : Active();
    if (ed) {
        int lines = std::max<int>(1, static_cast<int>(ed->Call(SCI_GETLINECOUNT)));
        int h = rc.bottom, w = rc.right;
        auto yOf = [&](int line) { return static_cast<int>(static_cast<int64_t>(line) * h / lines); };
        int first = static_cast<int>(ed->Call(SCI_DOCLINEFROMVISIBLE, ed->Call(SCI_GETFIRSTVISIBLELINE)));
        int shown = static_cast<int>(ed->Call(SCI_LINESONSCREEN));
        if (shown < lines) FillRectColor(mem, {0, yOf(first), w, std::max(yOf(first + shown), yOf(first) + 2)}, Theme::Hover);
        int markH = std::max(2, S(3));
        auto mark = [&](int line, COLORREF c, int left, int right) {
            int y = yOf(line);
            FillRectColor(mem, {left, y, right, std::min(h, y + markH)}, c);
        };
        for (int ind : {Editor::kIndicatorFind, Editor::kIndicatorOccurrence, Editor::kIndicatorSearch}) {
            Sci_Position len = ed->Call(SCI_GETLENGTH);
            Sci_Position pos = 0;
            for (int count = 0; pos < len && count < 2000; count++) {
                // INDICATOREND of a position outside the indicator is where the next run starts.
                Sci_Position start = ed->Call(SCI_INDICATORVALUEAT, ind, pos) ? pos : ed->Call(SCI_INDICATOREND, ind, pos);
                if (start <= pos && !ed->Call(SCI_INDICATORVALUEAT, ind, pos)) break;
                if (start >= len || !ed->Call(SCI_INDICATORVALUEAT, ind, start)) break;
                mark(static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, start)), Theme::Accent, w / 2, w);
                Sci_Position end = ed->Call(SCI_INDICATOREND, ind, start);
                if (end <= start) break;
                pos = end;
            }
        }
        for (const auto& [line, diags] : ed->Diagnostics()) {
            bool error = std::any_of(diags.begin(), diags.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; });
            mark(line - 1, error ? Theme::Error : Theme::Warning, 0, w);
        }
        for (const auto& [line, condition] : ed->Breakpoints()) {
            mark(line - 1, condition.empty() ? Theme::Breakpoint : Theme::ConditionalBreakpoint, 0, w / 2);
        }
        Sci_Position exec = ed->Call(SCI_MARKERNEXT, 0, 1 << Editor::kMarkerExecArrow);
        if (exec >= 0) mark(static_cast<int>(exec), Theme::ExecArrow, 0, w);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MainWindow::MarkBarClick(HWND bar, int y) {
    Editor* ed = bar == markBar2_ ? SplitEditor() : Active();
    if (!ed) return;
    RECT rc;
    GetClientRect(bar, &rc);
    int lines = static_cast<int>(ed->Call(SCI_GETLINECOUNT));
    int line = static_cast<int>(static_cast<int64_t>(std::clamp<int>(y, 0, rc.bottom)) * lines / std::max(1L, rc.bottom));
    ed->GotoLine(line + 1);
}
