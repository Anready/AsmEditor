#include "mw_internal.h"

namespace {

// Width of the mark bar: the scrollbar of the main editor, with the marks of the whole file.
constexpr int kMarkBarWidth = 10;  // as thick as the other scrollbars (scrollbar.cpp)
// Islands: the gap between the panels, the inset of the windows inside them and the corner radius (96-dpi pixels).
constexpr int kIslandGap = 6;
constexpr int kIslandPadding = 4;
constexpr int kIslandRadius = 10;
}

// ---- layout & painting ------------------------------------------------------------------------------------------

// An editor area with one tab group, or two (right of or below each other), each with a tab row above its editor.
void MainWindow::SplitGeometry(const RECT& area, bool split, bool down, RECT& tabs1, RECT& editor1, RECT& gap,
                               RECT& tabs2, RECT& editor2) const {
    int tabsH = S(34), w = S(5);
    if (split && down) {
        int mid = (area.top + area.bottom) / 2;
        tabs1 = {area.left, area.top, area.right, area.top + tabsH};
        editor1 = {area.left, tabs1.bottom, area.right, std::max<LONG>(tabs1.bottom, mid - w / 2)};
        gap = {area.left, editor1.bottom, area.right, editor1.bottom + w};
        tabs2 = {area.left, gap.bottom, area.right, gap.bottom + tabsH};
        editor2 = {area.left, tabs2.bottom, area.right, std::max(tabs2.bottom, area.bottom)};
    } else if (split) {
        int mid = (area.left + area.right) / 2;
        gap = {mid - w / 2, area.top, mid + w / 2, area.bottom};
        tabs1 = {area.left, area.top, gap.left, area.top + tabsH};
        editor1 = {area.left, tabs1.bottom, gap.left, std::max(tabs1.bottom, area.bottom)};
        tabs2 = {gap.right, area.top, area.right, area.top + tabsH};
        editor2 = {gap.right, tabs2.bottom, area.right, std::max(tabs2.bottom, area.bottom)};
    } else {
        tabs1 = {area.left, area.top, area.right, area.top + tabsH};
        editor1 = {area.left, tabs1.bottom, area.right, std::max(tabs1.bottom, area.bottom)};
        tabs2 = editor2 = gap = {};
    }
}

void MainWindow::Layout() {
    if (!tree_) return;
    FixGroups();
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

    // The debug panel on the right: one tab group, or two (the second below or right of the first) like the
    // editor's; at first the debugger's tabs above the memory view.
    bool panel = PanelShown();
    int debugW = panel ? std::clamp(S(settings_->debugWidth), S(220), std::max(S(220), W - x0 - S(300))) : 0;
    int x1 = panel ? W - g - debugW - g : W - g;  // right edge of the editor column
    rcSplitD_ = panel ? RECT{x1, bodyTop, x1 + g, bodyBottom} : RECT{};
    rcDebug_ = panel ? RECT{x1 + g, bodyTop, W - g, bodyBottom} : RECT{};
    for (FloatGroup& half : panel_) half.rcTabs = half.rcEditor = half.rcSplit = half.island = {};
    if (panel) {
        islands_.push_back(rcDebug_);
        panel_[0].island = panel_[1].island = rcDebug_;
        SplitGeometry({rcDebug_.left + p, rcDebug_.top, rcDebug_.right - p, rcDebug_.bottom - p},
                      GroupHasItems(kPanelGroup2), panelDown_, panel_[0].rcTabs, panel_[0].rcEditor, panel_[0].rcSplit,
                      panel_[1].rcTabs, panel_[1].rcEditor);
    }

    rcSplitH_ = {x0, bodyBottom - consoleH - g, x1, bodyBottom - consoleH};
    islands_.push_back({x0, bodyTop, x1, rcSplitH_.top});
    islands_.push_back({x0, rcSplitH_.bottom, x1, bodyBottom});
    // The editor; split, each half has a tab row of its own.
    SplitGeometry({x0 + p, bodyTop, x1 - p, rcSplitH_.top - p}, GroupHasItems(1), splitDown_, rcTabs_, rcEditor_,
                  rcSplitE_, rcTabs2_, rcEditor2_);
    rcConsoleHeader_ = {x0, rcSplitH_.bottom, x1, rcSplitH_.bottom + header};
    rcClear_ = {x1 - S(90), rcConsoleHeader_.top, x1 - S(12), rcConsoleHeader_.bottom};

    // Left panel: project tree or file outline.
    ShowWindow(tree_, leftTab_ == LeftTab::Project ? SW_SHOW : SW_HIDE);
    ShowWindow(outline_, leftTab_ == LeftTab::Outline ? SW_SHOW : SW_HIDE);
    HDWP dwp = BeginDeferWindowPos(8 + (int)editors_.size());
    dwp = DeferWindowPos(dwp, leftTab_ == LeftTab::Project ? tree_ : outline_, nullptr, treeIsland.left + p,
                         rcTreeHeader_.bottom, treeW - 2 * p, bodyBottom - p - rcTreeHeader_.bottom, SWP_NOZORDER | SWP_NOCOPYBITS);
    for (auto& view : views_) {
        dwp = DeferWindowPos(dwp, view->hwnd(), nullptr, x0 + p, rcConsoleHeader_.bottom, x1 - x0 - 2 * p,
                             bodyBottom - p - rcConsoleHeader_.bottom, SWP_NOZORDER | SWP_NOCOPYBITS);
    }
    EndDeferWindowPos(dwp);
    // Windows of their own: one island with one group, or two like the main editor.
    for (auto& [group, fg] : floatGroups_) {
        if (fg.second) continue;  // laid out with its first half
        RECT fc;
        GetClientRect(fg.hwnd, &fc);
        fg.island = {g, g, std::max<LONG>(g, fc.right - g), std::max<LONG>(g, fc.bottom - g)};
        FloatGroup* half = fg.partner ? &floatGroups_[fg.partner] : nullptr;
        RECT tabs2{}, editor2{};
        SplitGeometry({fg.island.left + p, fg.island.top, fg.island.right - p, fg.island.bottom - p}, half != nullptr,
                      fg.down, fg.rcTabs, fg.rcEditor, fg.rcSplit, tabs2, editor2);
        if (half) {
            half->island = fg.island;
            half->rcTabs = tabs2;
            half->rcEditor = editor2;
        }
        ShowWindow(fg.hwnd, WindowShown(group) ? SW_SHOWNA : SW_HIDE);
        InvalidateRect(fg.hwnd, nullptr, FALSE);
    }
    // Every file and part: the tab a group shows fills the group's area below its tab row.
    auto onScreen = [&](int group) {
        int primary = WindowPrimary(group);
        return primary == 0 || (primary == kPanelGroup ? panel : WindowShown(primary));
    };
    Editor *left = nullptr, *right = nullptr;  // in the main editor, with the mark bars
    for (size_t i = 0; i < editors_.size(); i++) {
        Editor* e = editors_[i].get();
        int group = e->group;
        bool shown = static_cast<int>(i) == ShownIndex(group) && ShownPart(group) < 0 && onScreen(group);
        // In the main editor the mark bar beside it is its scrollbar; elsewhere the one over its edge.
        ScrollBars::Show(e->hwnd(), group >= kPanelGroup, true);
        if (shown) {
            RECT r = GroupContent(group);
            int m = group < kPanelGroup ? mark : 0;
            SetWindowPos(e->hwnd(), nullptr, r.left, r.top, std::max<LONG>(0, r.right - r.left - m),
                         std::max<LONG>(0, r.bottom - r.top), SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOACTIVATE);
            if (group == 0) left = e;
            if (group == 1) right = e;
        }
        ShowWindow(e->hwnd(), shown ? SW_SHOW : SW_HIDE);
    }
    SetWindowPos(markBar_, nullptr, rcEditor_.right - mark, rcEditor_.top, mark, rcEditor_.bottom - rcEditor_.top,
                 SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOACTIVATE | (left ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    SetWindowPos(markBar2_, nullptr, rcEditor2_.right - mark, rcEditor2_.top, mark, rcEditor2_.bottom - rcEditor2_.top,
                 SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOACTIVATE | (right ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    for (int part = 0; part < kDebugParts; part++) {
        int group = partGroup_[part];
        if (ShownPart(group) == part && onScreen(group)) PlacePart(part, GroupContent(group));
        else HidePart(part);
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

// A part of the debugger shown in `content` of its group's window: the memory view below a row with the address
// and the format, the watches below the row to add one.
void MainWindow::PlacePart(int part, const RECT& content) {
    int inputH = S(36), w = content.right - content.left, top = content.top;
    HWND input = nullptr;
    int inputRight = content.right - S(16);
    if (part == kMemoryPart) {
        rcMemRow_ = {content.left, top, content.right, top + inputH};
        rcMemFormat_ = {content.right - S(110), top + S(6), content.right - S(8), top + inputH - S(6)};
        inputRight = rcMemFormat_.left - S(12);
        input = memExpr_;
    } else if (part == static_cast<int>(DebugTab::Watch)) {
        rcWatchRow_ = {content.left, top, content.right, top + inputH};
        input = watchExpr_;
    }
    if (input) {
        // The input sits inside a rounded frame that PaintPartChrome draws around it.
        SetWindowPos(input, nullptr, content.left + S(16), top + S(7), std::max(0, inputRight - (int)content.left - S(16)),
                     inputH - S(14), SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOACTIVATE);
        ShowWindow(input, SW_SHOW);
        top += inputH;
    }
    SetWindowPos(PartView(part)->hwnd(), nullptr, content.left, top, std::max(0, w),
                 std::max<LONG>(0, content.bottom - top), SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOACTIVATE);
    ShowWindow(PartView(part)->hwnd(), SW_SHOW);
}

void MainWindow::HidePart(int part) {
    ShowWindow(PartView(part)->hwnd(), SW_HIDE);
    if (part == kMemoryPart) {
        ShowWindow(memExpr_, SW_HIDE);
        rcMemRow_ = rcMemFormat_ = {};
    } else if (part == static_cast<int>(DebugTab::Watch)) {
        ShowWindow(watchExpr_, SW_HIDE);
        rcWatchRow_ = {};
    }
}
void MainWindow::InvalidateChrome() {
    InvalidateRect(hwnd_, &rcToolbar_, FALSE);
    InvalidateTabs();
    InvalidateRect(hwnd_, &rcStatus_, FALSE);
    InvalidateRect(hwnd_, &rcTreeHeader_, FALSE);
    InvalidateRect(hwnd_, &rcConsoleHeader_, FALSE);
    InvalidateRect(PartHost(kMemoryPart), &rcMemFormat_, FALSE);
    if (NoDockedEditors()) InvalidateRect(hwnd_, &rcEditor_, FALSE);
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

    // The splitters are the gaps between the islands; only split groups have a line of their own.
    auto splitLine = [&](const RECT& s, bool down) {
        if (down) {
            int mid = (s.top + s.bottom) / 2;
            FillRectColor(dc, {s.left + S(4), mid, s.right - S(4), mid + 1}, Theme::Border);
        } else {
            int mid = (s.left + s.right) / 2;
            FillRectColor(dc, {mid, s.top + S(4), mid + 1, s.bottom - S(4)}, Theme::Border);
        }
    };
    if (GroupHasItems(1)) splitLine(rcSplitE_, splitDown_);
    PaintTabs(dc, 0);
    if (GroupHasItems(1)) PaintTabs(dc, 1);
    if (NoDockedEditors()) PaintWelcome(dc);
    if (PanelShown()) {  // the debug panel: its groups, like the editor's
        PaintTabs(dc, kPanelGroup);
        if (GroupHasItems(kPanelGroup2)) {
            splitLine(panel_[0].rcSplit, panelDown_);
            PaintTabs(dc, kPanelGroup2);
        }
    }
    PaintPartChrome(dc, hwnd_);

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
        std::wstring text = stopped ? Tr(L"● отладка: остановлено") : Tr(L"● отладка: выполняется");
        if (stopped && !debugStatus_.empty()) text += L" — " + debugStatus_;  // the stop reason
        DrawTextColor(dc, text, state, stopped ? Theme::Warning : Theme::Success,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
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

// What a part shows besides its view, in `host`: the memory format beside the address row and the rounded frames
// around the inputs.
void MainWindow::PaintPartChrome(HDC dc, HWND host) {
    if (PartHost(kMemoryPart) == host && rcMemFormat_.right > rcMemFormat_.left) {
        static const wchar_t* formats[] = {L"байты", L"word", L"dword", L"qword", L"float", L"double", L"текст"};
        std::wstring format = std::wstring(Tr(formats[static_cast<int>(memFormat_)])) + L"  ▾";
        if (hoverMemFormat_) FillRound(dc, rcMemFormat_, S(6), Theme::Hover);
        SelectObject(dc, uiFont_);
        DrawTextColor(dc, format, rcMemFormat_, hoverMemFormat_ ? Theme::Bright : Theme::Text,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    for (HWND input : {memExpr_, watchExpr_}) {
        if (GetParent(input) != host || !IsWindowVisible(input)) continue;
        RECT frame;
        GetWindowRect(input, &frame);
        MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&frame), 2);
        InflateRect(&frame, S(6), S(4));
        FillRound(dc, frame, S(6), Theme::InputBg, GetFocus() == input ? Theme::Accent : Theme::Border);
    }
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
        if (((int)i == hoverTool_ || (int)i == openMenuTool_) && enabled) FillRound(dc, t.rc, S(6), Theme::Hover);
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

// The tab row of a group (in the window that holds it): its files, then the parts of the debugger in it.
void MainWindow::PaintTabs(HDC dc, int group) {
    if (tabRects_.size() != editors_.size()) {
        tabRects_.assign(editors_.size(), RECT{});
        closeRects_.assign(editors_.size(), RECT{});
    }
    std::vector<int> files = GroupTabs(group), parts = GroupParts(group);
    for (int i : files) tabRects_[i] = closeRects_[i] = RECT{};
    for (int part : parts) partTabRects_[part] = RECT{};
    RECT strip = StripRect(group);
    if ((files.empty() && parts.empty()) || strip.right <= strip.left) return;

    struct Tab {
        int file;  // editor index, or -1 for a part
        int part;
        int width;
    };
    std::vector<Tab> tabs;
    SelectObject(dc, uiFont_);
    for (int i : files) {
        Editor& ed = *editors_[i];
        std::wstring title = (ed.Modified() ? L"● " : L"") + ed.Title();
        tabs.push_back({i, -1, TextWidth(dc, title) + S(ed.pinned ? 82 : 70)});
    }
    for (int part : parts) tabs.push_back({-1, part, TextWidth(dc, PartTitle(part)) + S(30)});
    int shownPart = ShownPart(group), shownFile = shownPart < 0 ? ShownIndex(group) : -1;
    auto isShown = [&](const Tab& t) { return t.part >= 0 ? t.part == shownPart : t.file == shownFile; };
    int total = 0;
    for (const Tab& t : tabs) total += t.width;
    int avail = strip.right - strip.left;
    int& scroll = tabScroll_[group];
    if (ensureTabVisible_.count(group)) {
        int start = 0;
        for (const Tab& t : tabs) {
            if (isShown(t)) {
                if (start < scroll) scroll = start;
                if (start + t.width > scroll + avail) scroll = start + t.width - avail;
                break;
            }
            start += t.width;
        }
        ensureTabVisible_.erase(group);
    }
    scroll = std::clamp(scroll, 0, std::max(0, total - avail));
    // The group with the focus underlines its tab in the accent color.
    bool groupFocused = group >= kPanelGroup ? floatFocus_ == group
                                             : floatFocus_ == 0 && (group == 1) == (focusRight_ && GroupHasItems(1));

    HRGN clip = CreateRectRgn(strip.left, strip.top, strip.right, strip.bottom);
    SelectClipRgn(dc, clip);
    int x = strip.left - scroll;
    for (const Tab& t : tabs) {
        RECT r{x, strip.top, x + t.width, strip.bottom};
        x += t.width;
        // IntelliJ-style tabs: no boxes, the open tab is underlined (in the accent color while it has the focus),
        // the one under the mouse gets a rounded highlight.
        bool open = isShown(t);
        bool dragged = drag_ == Drag::Tab && dragMoved_ && (t.part >= 0 ? t.part == dragPart_ : t.file == dragTab_);
        bool hover = t.part >= 0 ? t.part == hoverPart_ : t.file == hoverTab_;
        RECT pill{r.left + S(2), r.top + S(5), r.right - S(2), r.bottom - S(5)};
        if (dragged) FillRound(dc, pill, S(6), Theme::Hover, Theme::Accent);
        else if (!open && hover) FillRound(dc, pill, S(6), Theme::Hover);
        if (open) {
            FillRound(dc, {r.left + S(6), r.bottom - S(3), r.right - S(6), r.bottom}, S(1),
                      open && groupFocused ? Theme::Accent : Theme::Dim);
        }
        if (t.part >= 0) {  // a part of the debugger: its name, no close button
            partTabRects_[t.part] = r;
            SelectObject(dc, uiFont_);
            DrawTextColor(dc, PartTitle(t.part), {r.left + S(15), r.top, r.right - S(8), r.bottom},
                          open ? Theme::Bright : (hover ? Theme::Text : Theme::Muted), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            continue;
        }
        int i = t.file;
        Editor& ed = *editors_[i];
        tabRects_[i] = r;
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
        // The icon of the file type, as in IntelliJ.
        int iconSize = S(16);
        FileIcons::Draw(dc, textLeft, (r.top + r.bottom - iconSize) / 2, iconSize,
                        FileIcons::KindOf(ed.file().empty() ? fs::path(ed.Title()) : ed.file()));
        textLeft += iconSize + S(6);
        RECT tr{textLeft, r.top, r.right - S(28), r.bottom};
        SelectObject(dc, isMain ? uiBold_ : uiFont_);
        DrawTextColor(dc, title, tr, errors ? Theme::Error : (open ? Theme::Bright : Theme::Text),
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        int cy = (r.top + r.bottom) / 2;
        RECT cr{r.right - S(28), cy - S(9), r.right - S(10), cy + S(9)};
        closeRects_[i] = cr;
        if (i == hoverClose_) FillRound(dc, cr, S(4), Theme::Pressed);
        if (open || hover) {
            SelectObject(dc, uiFont_);
            DrawTextColor(dc, L"✕", cr, i == hoverClose_ ? Theme::Bright : Theme::Muted,
                          DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    SelectObject(dc, uiFont_);
}

// A window of its own: the island with its tab rows (the views cover the rest).
void MainWindow::PaintFloat(HDC dc, int group) {
    auto it = floatGroups_.find(group);
    if (it == floatGroups_.end()) return;
    const FloatGroup& fg = it->second;
    RECT rc;
    GetClientRect(fg.hwnd, &rc);
    FillRectColor(dc, rc, Theme::FrameBg);
    FillRound(dc, fg.island, S(kIslandRadius), Theme::EditorBg);
    SetBkMode(dc, TRANSPARENT);
    PaintTabs(dc, group);
    if (fg.partner) {  // split: the second tab row and the line between the halves
        PaintTabs(dc, fg.partner);
        const RECT& s = fg.rcSplit;
        if (fg.down) {
            int mid = (s.top + s.bottom) / 2;
            FillRectColor(dc, {s.left + S(4), mid, s.right - S(4), mid + 1}, Theme::Border);
        } else {
            int mid = (s.left + s.right) / 2;
            FillRectColor(dc, {mid, s.top + S(4), mid + 1, s.bottom - S(4)}, Theme::Border);
        }
    }
    PaintPartChrome(dc, fg.hwnd);
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

    // Right: the toolchain, problems, position, encoding, line ends.
    std::vector<std::pair<std::wstring, COLORREF>> parts;
    if (!toolStatus_.empty()) parts.push_back({toolStatus_, toolStatusWarn_ ? Theme::Warning : Theme::Muted});
    Editor* ed = Current();
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

    // Left: the breadcrumbs; when they do not fit, the outer ones give way to "…".
    crumbs_ = Breadcrumbs(ed);
    int iconSize = S(16), sep = TextWidth(dc, L"  ›  "), avail = x - S(14) - S(10);
    auto width = [&](const Crumb& c) { return TextWidth(dc, c.text) + (c.file ? iconSize + S(4) : 0) + S(8); };
    auto total = [&] {
        int t = 0;
        for (const Crumb& c : crumbs_) t += width(c) + sep;
        return t - sep;
    };
    bool cut = false;
    while (crumbs_.size() > 1 && total() > avail) {
        crumbs_.erase(crumbs_.begin());
        cut = true;
    }
    if (cut) crumbs_.insert(crumbs_.begin(), Crumb{L"…"});
    int cx = rcStatus_.left + S(10);
    for (size_t i = 0; i < crumbs_.size(); i++) {
        Crumb& c = crumbs_[i];
        int w = width(c);
        c.rc = {cx, rcStatus_.top + S(3), cx + w, rcStatus_.bottom - S(3)};
        if (static_cast<int>(i) == hoverCrumb_ && c.line >= 0) FillRound(dc, c.rc, S(4), Theme::Hover);
        int tx = cx + S(4);
        if (c.file) {
            FileIcons::Draw(dc, tx, (rcStatus_.top + rcStatus_.bottom - iconSize) / 2, iconSize,
                            FileIcons::KindOf(ed && !ed->file().empty() ? ed->file() : fs::path(c.text)));
            tx += iconSize + S(4);
        }
        bool last = i + 1 == crumbs_.size();
        DrawTextColor(dc, c.text, {tx, rcStatus_.top, cx + w, rcStatus_.bottom}, last ? Theme::Text : Theme::Muted,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        cx += w;
        if (!last) {
            DrawTextColor(dc, L"  ›  ", {cx, rcStatus_.top, cx + sep, rcStatus_.bottom}, Theme::Dim, DT_SINGLELINE | DT_VCENTER);
            cx += sep;
        }
    }
}

int MainWindow::CrumbAt(int x, int y) const {
    for (size_t i = 0; i < crumbs_.size(); i++) {
        if (crumbs_[i].line >= 0 && Contains(crumbs_[i].rc, x, y)) return static_cast<int>(i);
    }
    return -1;
}

std::vector<MainWindow::Crumb> MainWindow::Breadcrumbs(Editor* ed) const {
    std::vector<Crumb> out;
    if (!ed) return out;
    if (!ed->file().empty() && !projectRoot_.empty() && IsUnder(ed->file(), projectRoot_)) {
        out.push_back({projectRoot_.filename().wstring()});
        fs::path rel = NormalizePath(ed->file()).lexically_relative(NormalizePath(projectRoot_)).parent_path();
        for (const fs::path& part : rel) {
            if (!part.empty() && part != L".") out.push_back({part.wstring()});
        }
    }
    out.push_back({ed->Title(), -1, true});

    // What the caret is in: the fold parents (global label, %macro/%if/%rep), the section above, the local label.
    int caret = ed->CaretLine() - 1;
    std::vector<int> lines;
    for (Sci_Position p = caret; (p = ed->Call(SCI_GETFOLDPARENT, p)) >= 0 && lines.size() < 16;) lines.push_back(static_cast<int>(p));
    auto firstWord = [&](int line) {
        std::string t = ed->LineText(line);
        size_t a = t.find_first_not_of(" \t[");
        if (a == std::string::npos) return std::string();
        size_t b = t.find_first_of(" \t:;]", a);
        std::string w = t.substr(a, b == std::string::npos ? std::string::npos : b - a);
        for (char& c : w) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return w;
    };
    static const std::set<std::string> kSections = {"section", "segment", ".section", ".text", ".data", ".bss", ".rodata", ".code"};
    for (int l = caret; l >= 0 && caret - l < 20000; l--) {
        if (kSections.count(firstWord(l))) {
            lines.push_back(l);
            break;
        }
    }
    int global = -1;
    for (int l : lines) {
        std::string w = firstWord(l);
        if (!w.empty() && w[0] != '%' && w[0] != '.' && !kSections.count(w)) global = std::max(global, l);
    }
    if (global >= 0) {
        for (int l = caret; l > global; l--) {
            std::string t = ed->LineText(l);
            if (!t.empty() && t[0] == '.' && t.find(':') != std::string::npos) {
                lines.push_back(l);
                break;
            }
        }
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    for (int l : lines) {
        std::string t = ed->LineText(l);
        t = t.substr(0, t.find(';'));
        std::string text;
        for (char c : t) {
            if (c == '\t' || c == '\r' || c == '\n') c = ' ';
            if (c == ' ' && (text.empty() || text.back() == ' ')) continue;
            text += c;
        }
        while (!text.empty() && (text.back() == ' ' || text.back() == ':')) text.pop_back();
        if (text.size() > 40) text = text.substr(0, 39) + "…";
        if (!text.empty()) out.push_back({Widen(text), l});
    }
    return out;
}

void MainWindow::OnMouseMove(int x, int y) {
    if (drag_ == Drag::Tree || drag_ == Drag::Console || drag_ == Drag::Debug) {
        if (drag_ == Drag::Tree) settings_->treeWidth = std::max(80, MulDiv(x - S(kIslandGap), 96, dpi_));
        if (drag_ == Drag::Console) settings_->consoleHeight = std::max(60, MulDiv(rcStatus_.top - y, 96, dpi_));
        if (drag_ == Drag::Debug) {
            settings_->debugWidth = std::max(180, MulDiv(rcStatus_.right - S(2 * kIslandGap) - x, 96, dpi_));
        }
        Layout();
        // Repaint at once, children included: with fast moves the uncovered strips would otherwise show what was
        // there before until the mouse stops.
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        return;
    }
    if (drag_ == Drag::Tab) {
        TabDragMove(hwnd_, x, y);
        return;
    }
    if (int crumb = CrumbAt(x, y); crumb != hoverCrumb_) {
        hoverCrumb_ = crumb;
        InvalidateRect(hwnd_, &rcStatus_, FALSE);
    }
    int tool = -1, close = -1, link = -1, pane = -1, leftTab = -1;
    for (size_t i = 0; i < tools_.size(); i++) if (Contains(tools_[i].rc, x, y)) tool = (int)i;
    bool onClose;
    int tab = TabAt(hwnd_, x, y, &onClose);
    if (onClose) close = tab;
    int part = PartTabAt(hwnd_, x, y);
    if (NoDockedEditors()) {
        for (size_t i = 0; i < welcomeLinks_.size(); i++) if (Contains(welcomeLinks_[i].first, x, y)) link = (int)i;
    }
    for (size_t i = 0; i < paneRects_.size(); i++) if (Contains(paneRects_[i], x, y)) pane = (int)i;
    for (size_t i = 0; i < leftTabRects_.size(); i++) if (Contains(leftTabRects_[i], x, y)) leftTab = (int)i;
    bool clear = Contains(rcClear_, x, y);
    bool refresh = !projectRoot_.empty() && Contains(rcRefresh_, x, y);
    // The memory format in a window of its own tracks its hover itself.
    bool memFormat = PartHost(kMemoryPart) == hwnd_ ? Contains(rcMemFormat_, x, y) : hoverMemFormat_;
    if (tool != hoverTool_ || tab != hoverTab_ || close != hoverClose_ || link != hoverLink_ || clear != hoverClear_ ||
        refresh != hoverRefresh_ || pane != hoverPane_ || leftTab != hoverLeftTab_ || part != hoverPart_ ||
        memFormat != hoverMemFormat_) {
        if (tool != hoverTool_) {  // a menu title opens after a short pause under the mouse
            KillTimer(hwnd_, kMenuHoverTimer);
            if (tool >= 0 && tools_[tool].menu) SetTimer(hwnd_, kMenuHoverTimer, 250, nullptr);
        }
        hoverTool_ = tool;
        hoverTab_ = tab;
        hoverClose_ = close;
        hoverLink_ = link;
        hoverClear_ = clear;
        hoverRefresh_ = refresh;
        hoverPane_ = pane;
        hoverLeftTab_ = leftTab;
        hoverPart_ = part;
        hoverMemFormat_ = memFormat;
        InvalidateChrome();
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
    TrackMouseEvent(&tme);
}

void MainWindow::OnClick(int x, int y, bool middle) {
    bool onClose;
    if (int tab = TabAt(hwnd_, x, y, &onClose); tab >= 0) {
        if ((middle && !editors_[tab]->pinned) || onClose) CloseEditor(tab);
        else if (!middle) Activate(tab);
        return;
    }
    if (middle) return;
    for (size_t i = 0; i < tools_.size(); i++) {
        const ToolItem& t = tools_[i];
        if (!Contains(t.rc, x, y)) continue;
        if (t.menu) {
            OpenToolMenu(static_cast<int>(i));
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
    if (MemFormatClick(hwnd_, x, y)) return;
    if (Contains(rcClear_, x, y)) OnCommand(ID_CLEAR_CONSOLE);
    else if (!projectRoot_.empty() && Contains(rcRefresh_, x, y)) RefreshTree();
    else if (NoDockedEditors()) {
        for (auto& [r, path] : welcomeLinks_) {
            if (Contains(r, x, y)) {
                OpenPath(path);
                return;
            }
        }
    }
}

// The project widget's menu: the recent projects (the open one checked) and "Open folder".
namespace {

MainWindow* g_menuOwner = nullptr;  // the window whose title menu is open (the hook below has no context)

}  // namespace

// While a title menu is open, the mouse over another title closes it and opens that one, as in a menu bar.
LRESULT CALLBACK MainWindow::MenuFilter(int code, WPARAM wp, LPARAM lp) {
    auto* msg = reinterpret_cast<MSG*>(lp);
    if (code == MSGF_MENU && msg->message == WM_MOUSEMOVE && g_menuOwner) {
        MainWindow& w = *g_menuOwner;
        POINT p = msg->pt;
        ScreenToClient(w.hwnd_, &p);
        for (size_t i = 0; i < w.tools_.size(); i++) {
            if (!w.tools_[i].menu || !Contains(w.tools_[i].rc, p.x, p.y) || (int)i == w.openMenuTool_) continue;
            w.nextMenuTool_ = static_cast<int>(i);
            EndMenu();
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// True while the mouse is over the open title or over the menu (its submenus included).
bool MainWindow::MouseInToolMenu() const {
    POINT c;
    GetCursorPos(&c);
    wchar_t cls[16] = {};
    if (HWND h = WindowFromPoint(c); h && GetClassNameW(h, cls, 16) && wcscmp(cls, L"#32768") == 0) return true;
    ScreenToClient(hwnd_, &c);
    return openMenuTool_ >= 0 && Contains(tools_[openMenuTool_].rc, c.x, c.y);
}

void MainWindow::OpenToolMenu(int index, bool byHover) {
    KillTimer(hwnd_, kMenuHoverTimer);
    while (index >= 0) {
        const ToolItem& t = tools_[index];
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
        // The open title stays highlighted (see the toolbar painting) until its menu closes.
        openMenuTool_ = index;
        nextMenuTool_ = -1;
        hoverTool_ = index;
        InvalidateChrome();
        UpdateWindow(hwnd_);
        POINT p{t.rc.left, t.rc.bottom};
        ClientToScreen(hwnd_, &p);
        g_menuOwner = this;
        HHOOK hook = SetWindowsHookExW(WH_MSGFILTER, MenuFilter, nullptr, GetCurrentThreadId());
        // A menu opened by hovering closes once the mouse has been away from it for a moment.
        menuAwayTicks_ = 0;
        if (byHover) SetTimer(hwnd_, kMenuLeaveTimer, 100, nullptr);
        TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, hwnd_, nullptr);
        KillTimer(hwnd_, kMenuLeaveTimer);
        if (hook) UnhookWindowsHookEx(hook);
        g_menuOwner = nullptr;
        openMenuTool_ = -1;
        index = nextMenuTool_;
    }
    // The title still under the mouse counts as hovered already, so it does not open again by itself.
    POINT c;
    GetCursorPos(&c);
    ScreenToClient(hwnd_, &c);
    hoverTool_ = -1;
    for (size_t i = 0; i < tools_.size(); i++) if (Contains(tools_[i].rc, c.x, c.y)) hoverTool_ = (int)i;
    InvalidateChrome();
}

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
    if (int tab = TabAt(hwnd_, x, y); tab >= 0) TabMenu(tab);
    else if (int part = PartTabAt(hwnd_, x, y); part >= 0) PartMenu(part);
}

// A click on the memory format, in whichever window shows the memory view; true when it was handled.
bool MainWindow::MemFormatClick(HWND host, int x, int y) {
    if (host != PartHost(kMemoryPart) || !Contains(rcMemFormat_, x, y)) return false;
    HMENU menu = CreatePopupMenu();
    static const wchar_t* formats[] = {L"байты", L"word (2 байта)", L"dword (4 байта)", L"qword (8 байт)",
                                       L"float", L"double", L"текст"};
    for (int i = 0; i < static_cast<int>(MemFormat::Count); i++) {
        AppendMenuW(menu, MF_STRING | (i == static_cast<int>(memFormat_) ? MF_CHECKED : 0), i + 1, Tr(formats[i]));
    }
    POINT p{rcMemFormat_.left, rcMemFormat_.bottom};
    ClientToScreen(host, &p);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, host, nullptr);
    DestroyMenu(menu);
    if (cmd > 0) {
        memFormat_ = static_cast<MemFormat>(cmd - 1);
        prevMem_.clear();
        RenderMemory();
        InvalidateRect(host, &rcMemFormat_, FALSE);
    }
    return true;
}

// Hover over the memory format in a window of its own.
void MainWindow::MemFormatHover(HWND host, int x, int y) {
    if (host != PartHost(kMemoryPart)) return;
    bool format = x >= 0 && Contains(rcMemFormat_, x, y);
    if (format == hoverMemFormat_) return;
    hoverMemFormat_ = format;
    InvalidateRect(host, &rcMemFormat_, FALSE);
}
void MainWindow::UpdateTitle() {
    std::wstring title;
    if (!projectRoot_.empty()) title += projectRoot_.filename().wstring() + L" – ";
    if (Editor* ed = Active()) title += ed->Title() + L" – ";
    title += L"AsmEditor";
    SetWindowTextW(hwnd_, title.c_str());
    UpdateFloatTitles();
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
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_MOUSELEAVE:
    case WM_CAPTURECHANGED:
        if (self) self->MarkBarMouse(hwnd, msg, GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL:  // scrolls the editor
        if (Editor* ed = self ? self->MarkBarEditor(hwnd) : nullptr) return SendMessageW(ed->hwnd(), msg, wp, lp);
        return 0;
    case WM_SETCURSOR:
        if (self && LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            Editor* ed = self->MarkBarEditor(hwnd);
            bool onMark = ed && self->MarkBarMarkAt(hwnd, ed, p.y) >= 0;
            SetCursor(LoadCursorW(nullptr, onMark ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

Editor* MainWindow::MarkBarEditor(HWND bar) const {
    return bar == markBar2_ ? SplitEditor() : Active();
}

// The thumb of the scrollbar: the visible part of the scroll range (the lines plus a few past the end).
RECT MainWindow::MarkBarThumb(HWND bar, Editor* ed) const {
    RECT rc;
    GetClientRect(bar, &rc);
    int64_t total = std::max<int64_t>(1, ed->ScrollLines()), shown = ed->Call(SCI_LINESONSCREEN);
    double top = ScrollBars::Position(ed->hwnd());  // with the part of a line while scrolling smoothly
    if (shown >= total) return {};  // everything fits: no thumb
    int h = rc.bottom, minH = S(24);
    int thumbH = std::max<int>(minH, static_cast<int>(shown * h / total));
    int y = static_cast<int>(std::lround(top * (h - thumbH) / static_cast<double>(std::max<int64_t>(1, total - shown))));
    return {S(2), y, rc.right - S(2), y + thumbH};
}

// The line of a mark (problem, breakpoint, execution line) within a few pixels of y, else -1.
int MainWindow::MarkBarMarkAt(HWND bar, Editor* ed, int y) const {
    RECT rc;
    GetClientRect(bar, &rc);
    int64_t total = std::max<int64_t>(1, std::max<int64_t>(ed->ScrollLines(), ed->Call(SCI_LINESONSCREEN)));
    auto yOf = [&](int line) { return static_cast<int>(ed->Call(SCI_VISIBLEFROMDOCLINE, line) * rc.bottom / total); };
    std::vector<int> lines;
    for (const auto& [line, diags] : ed->Diagnostics()) lines.push_back(line - 1);
    for (const auto& [line, condition] : ed->Breakpoints()) lines.push_back(line - 1);
    Sci_Position exec = ed->Call(SCI_MARKERNEXT, 0, 1 << Editor::kMarkerExecArrow);
    if (exec >= 0) lines.push_back(static_cast<int>(exec));
    int best = -1, bestDistance = S(4);
    for (int line : lines) {
        int d = std::abs(yOf(line) + S(1) - y);
        if (d <= bestDistance) {
            best = line;
            bestDistance = d;
        }
    }
    return best;
}

// The editor's scrollbar, as in IntelliJ: a rounded thumb over the marks of the whole file (problems,
// breakpoints, the execution line, search hits). Dragging the thumb scrolls; clicking a mark goes to its line,
// clicking elsewhere moves the thumb there.
void MainWindow::PaintMarkBar(HWND bar, HDC dc) {
    RECT rc;
    GetClientRect(bar, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ old = SelectObject(mem, bmp);
    FillRectColor(mem, rc, Theme::EditorBg);
    if (Editor* ed = MarkBarEditor(bar)) {
        int h = rc.bottom, w = rc.right;
        RECT thumb = MarkBarThumb(bar, ed);
        if (thumb.bottom > thumb.top) {
            bool active = markDrag_ == bar || markHover_ == bar;
            FillRound(mem, thumb, S(4), Theme::Blend(Theme::EditorBg, Theme::Muted, active ? 55 : 30));
        }
        int64_t total = std::max<int64_t>(1, std::max<int64_t>(ed->ScrollLines(), ed->Call(SCI_LINESONSCREEN)));
        auto yOf = [&](int line) { return static_cast<int>(ed->Call(SCI_VISIBLEFROMDOCLINE, line) * h / total); };
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
                mark(static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, start)), Theme::Accent, w / 2, w - S(2));
                Sci_Position end = ed->Call(SCI_INDICATOREND, ind, start);
                if (end <= start) break;
                pos = end;
            }
        }
        for (const auto& [line, diags] : ed->Diagnostics()) {
            bool error = std::any_of(diags.begin(), diags.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; });
            mark(line - 1, error ? Theme::Error : Theme::Warning, S(2), w - S(2));
        }
        for (const auto& [line, condition] : ed->Breakpoints()) {
            mark(line - 1, condition.empty() ? Theme::Breakpoint : Theme::ConditionalBreakpoint, S(2), w / 2);
        }
        Sci_Position exec = ed->Call(SCI_MARKERNEXT, 0, 1 << Editor::kMarkerExecArrow);
        if (exec >= 0) mark(static_cast<int>(exec), Theme::ExecArrow, S(2), w - S(2));
        // Changed lines (as in the change margin): a thin stripe on the left.
        const std::pair<int, COLORREF> changes[] = {{Editor::kMarkerAdded, Theme::Success},
                                                    {Editor::kMarkerModified, Theme::Link},
                                                    {Editor::kMarkerDeleted, Theme::Muted}};
        for (auto [marker, color] : changes) {
            int count = 0;
            sptr_t mask = sptr_t{1} << marker;
            for (Sci_Position l = ed->Call(SCI_MARKERNEXT, 0, mask); l >= 0 && count < 5000;
                 l = ed->Call(SCI_MARKERNEXT, l + 1, mask), count++) {
                mark(static_cast<int>(l), Theme::Blend(Theme::EditorBg, color, 70), 0, S(2));
            }
        }
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MainWindow::MarkBarMouse(HWND bar, UINT msg, int y) {
    if (msg == WM_MOUSELEAVE || msg == WM_CAPTURECHANGED) {
        if (msg == WM_CAPTURECHANGED) markDrag_ = nullptr;
        else markHover_ = nullptr;
        InvalidateRect(bar, nullptr, FALSE);
        return;
    }
    Editor* ed = MarkBarEditor(bar);
    if (!ed) return;
    RECT rc;
    GetClientRect(bar, &rc);
    RECT thumb = MarkBarThumb(bar, ed);
    int64_t total = ed->ScrollLines(), shown = ed->Call(SCI_LINESONSCREEN);
    // Puts the thumb's top at thumbTop (as the first visible line).
    auto scrollTo = [&](int thumbTop) {
        int track = std::max<int>(1, rc.bottom - (thumb.bottom - thumb.top));
        int64_t top = static_cast<int64_t>(std::clamp(thumbTop, 0, track)) * std::max<int64_t>(0, total - shown) / track;
        ed->Call(SCI_SETFIRSTVISIBLELINE, top);
    };
    bool onThumb = y >= thumb.top && y < thumb.bottom;
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int line = MarkBarMarkAt(bar, ed, y);
        if (line >= 0 && !onThumb) {  // a mark: go to its line
            ed->GotoLine(line + 1);
            SetFocus(ed->hwnd());
            return;
        }
        if (thumb.bottom <= thumb.top) return;
        // On the thumb: drag it from where it was taken; beside it: the thumb jumps there (centered) and drags.
        markGrab_ = onThumb ? y - thumb.top : (thumb.bottom - thumb.top) / 2;
        if (!onThumb) scrollTo(y - markGrab_);
        markDrag_ = bar;
        SetCapture(bar);
        InvalidateRect(bar, nullptr, FALSE);
        return;
    }
    case WM_MOUSEMOVE:
        if (markDrag_ == bar) {
            scrollTo(y - markGrab_);
            UpdateWindow(ed->hwnd());
        } else if (markHover_ != bar) {
            markHover_ = bar;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, bar, 0};
            TrackMouseEvent(&tme);
        }
        InvalidateRect(bar, nullptr, FALSE);
        return;
    case WM_LBUTTONUP:
        if (markDrag_ == bar) {
            markDrag_ = nullptr;
            ReleaseCapture();
            InvalidateRect(bar, nullptr, FALSE);
        }
        return;
    }
}
