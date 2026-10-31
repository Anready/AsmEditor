#include "mw_internal.h"

// ---- tab groups and windows of their own --------------------------------------------------------------------------
// Every window is made of one or two tab groups, and every group holds files and parts of the debugger alike: the
// main editor (groups 0 and 1), the debug panel (2 and 3) and windows of their own (>= kFirstFloatGroup, e.g. on a
// second monitor). The views stay the same objects: their windows get a new parent, and a window of its own passes
// their notifications on to the main window.

namespace {

constexpr wchar_t kFloatClass[] = L"AsmEditorFloat";
constexpr wchar_t kDropHintClass[] = L"AsmEditorDropHint";
constexpr UINT WM_FLOAT_DOCK = WM_APP + 1;  // posted to a window dropped on the main one: wp, lp = cursor

LRESULT CALLBACK DropHintProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRectColor(reinterpret_cast<HDC>(wp), rc, Theme::Accent);
        return 1;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

RECT Union(const RECT& a, const RECT& b) {
    RECT r;
    UnionRect(&r, &a, &b);
    return r;
}

}  // namespace

// ---- groups -------------------------------------------------------------------------------------------------------

MainWindow::FloatGroup* MainWindow::GroupInfo(int group) {
    if (group == kPanelGroup || group == kPanelGroup2) return &panel_[group - kPanelGroup];
    auto it = floatGroups_.find(group);
    return it == floatGroups_.end() ? nullptr : &it->second;
}

const MainWindow::FloatGroup* MainWindow::GroupInfo(int group) const {
    return const_cast<MainWindow*>(this)->GroupInfo(group);
}

Editor* MainWindow::Current() const {
    if (floatFocus_ >= kPanelGroup && ShownPart(floatFocus_) < 0) {
        if (const FloatGroup* info = GroupInfo(floatFocus_)) {
            if (Editor* ed = FindById(info->shown)) return ed;
        }
    }
    return focusRight_ && SplitEditor() ? SplitEditor() : Active();
}

HWND MainWindow::FloatOf(const Editor* ed) const {
    if (!IsFloating(ed)) return nullptr;
    const FloatGroup* info = GroupInfo(ed->group);
    return info ? info->hwnd : nullptr;
}

int MainWindow::FloatGroupOf(HWND f) const {
    for (auto& [group, fg] : floatGroups_) {
        if (fg.hwnd == f && !fg.second) return group;
    }
    return 0;
}

bool MainWindow::IsFloatWindow(HWND root) const {
    return root && FloatGroupOf(root) != 0;
}

std::vector<int> MainWindow::GroupTabs(int group) const {
    std::vector<int> out;
    for (size_t i = 0; i < editors_.size(); i++) {
        if (editors_[i]->group == group) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::vector<int> MainWindow::GroupParts(int group) const {
    std::vector<int> out;
    for (int part = 0; part < kDebugParts; part++) {
        if (partGroup_[part] == group) out.push_back(part);
    }
    return out;
}

int MainWindow::GroupItemCount(int group) const {
    return static_cast<int>(GroupTabs(group).size() + GroupParts(group).size());
}

bool MainWindow::GroupHasItems(int group) const {
    return GroupItemCount(group) > 0;
}

bool MainWindow::NoDockedEditors() const {
    return !GroupHasItems(0);
}

// A window with files is always shown; one with only parts of the debugger while the debug panel would be.
bool MainWindow::WindowShown(int primary) const {
    if (primary == 0) return true;
    int second = SecondHalf(primary);
    for (auto& e : editors_) {
        if (e->group == primary || (second && e->group == second)) return true;
    }
    return DebugPanelVisible();
}

bool MainWindow::PanelShown() const {
    return (GroupHasItems(kPanelGroup) || GroupHasItems(kPanelGroup2)) && WindowShown(kPanelGroup);
}

HWND MainWindow::GroupHost(int group) const {
    if (group < kFirstFloatGroup) return hwnd_;
    const FloatGroup* info = GroupInfo(group);
    return info ? info->hwnd : nullptr;
}

RECT MainWindow::StripRect(int group) const {
    if (group == 0) return rcTabs_;
    if (group == 1) return rcTabs2_;
    const FloatGroup* info = GroupInfo(group);
    return info ? info->rcTabs : RECT{};
}

RECT MainWindow::GroupContent(int group) const {
    if (group == 0) return rcEditor_;
    if (group == 1) return rcEditor2_;
    const FloatGroup* info = GroupInfo(group);
    return info ? info->rcEditor : RECT{};
}

RECT MainWindow::GroupArea(int group) const {
    return Union(StripRect(group), GroupContent(group));
}

int MainWindow::ShownIndex(int group) const {
    if (group == 0) return active_;
    if (group == 1) return splitIndex_;
    const FloatGroup* info = GroupInfo(group);
    return info ? IndexOf(FindById(info->shown)) : -1;
}

int MainWindow::ShownPart(int group) const {
    auto it = shownPart_.find(group);
    if (it == shownPart_.end() || it->second < 0 || it->second >= kDebugParts) return -1;
    return partGroup_[it->second] == group ? it->second : -1;
}

int MainWindow::NeighbourInGroup(int index) const {
    int n = static_cast<int>(editors_.size());
    if (index < 0 || index >= n) return -1;
    int group = editors_[index]->group;
    for (int d = 1; d < n; d++) {
        for (int i : {index - d, index + d}) {
            if (i >= 0 && i < n && editors_[i]->group == group) return i;
        }
    }
    return -1;
}

int MainWindow::WindowPrimary(int group) const {
    if (group < kPanelGroup) return 0;
    if (group < kFirstFloatGroup) return kPanelGroup;
    const FloatGroup* info = GroupInfo(group);
    return info && info->second ? info->partner : group;
}

int MainWindow::SecondHalf(int primary) const {
    if (primary == 0) return GroupHasItems(1) ? 1 : 0;
    if (primary == kPanelGroup) return GroupHasItems(kPanelGroup2) ? kPanelGroup2 : 0;
    const FloatGroup* info = GroupInfo(primary);
    return info ? info->partner : 0;
}

bool MainWindow::IsSecondHalf(int group) const {
    if (group == 1 || group == kPanelGroup2) return true;
    const FloatGroup* info = group >= kFirstFloatGroup ? GroupInfo(group) : nullptr;
    return info && info->second;
}

// The second group of a window (made when it is not split yet), right of or below the first one.
int MainWindow::EnsureSecondHalf(int primary, bool down) {
    if (primary == 0) {
        splitDown_ = down;
        return 1;
    }
    if (primary == kPanelGroup) {
        panelDown_ = down;
        return kPanelGroup2;
    }
    if (!floatGroups_.count(primary)) return primary;
    if (!floatGroups_[primary].partner) {
        int second = nextGroup_++;
        FloatGroup half;
        half.hwnd = floatGroups_[primary].hwnd;
        half.partner = primary;
        half.second = true;
        floatGroups_[second] = half;
        floatGroups_[primary].partner = second;
    }
    floatGroups_[primary].down = down;
    return floatGroups_[primary].partner;
}

void MainWindow::FixGroups() {
    int n = static_cast<int>(editors_.size());
    for (auto& e : editors_) {  // a window that is gone: back to the main window
        if (e->group >= kFirstFloatGroup && !floatGroups_.count(e->group)) e->group = 0;
    }
    for (int& group : partGroup_) {
        if (group < 0 || (group >= kFirstFloatGroup && !floatGroups_.count(group))) group = kPanelGroup;
    }
    auto moveAll = [&](int from, int to) {
        for (auto& e : editors_) {
            if (e->group == from) e->group = to;
        }
        for (int& group : partGroup_) {
            if (group == from) group = to;
        }
        auto it = shownPart_.find(from);
        shownPart_[to] = it != shownPart_.end() ? it->second : -1;
    };
    // A first half left without tabs takes those of its second half.
    if (!GroupHasItems(0) && GroupHasItems(1)) {
        moveAll(1, 0);
        active_ = splitIndex_;
        splitIndex_ = -1;
    }
    if (!GroupHasItems(kPanelGroup) && GroupHasItems(kPanelGroup2)) {
        moveAll(kPanelGroup2, kPanelGroup);
        panel_[0].shown = panel_[1].shown;
    }
    for (auto& [group, fg] : floatGroups_) {
        if (fg.second || !fg.partner || GroupHasItems(group)) continue;
        moveAll(fg.partner, group);
        fg.shown = floatGroups_[fg.partner].shown;
    }
    for (int i = 0; i < 2; i++) {
        panel_[i].hwnd = hwnd_;
        panel_[i].second = i == 1;
    }
    panel_[0].partner = GroupHasItems(kPanelGroup2) ? kPanelGroup2 : 0;
    panel_[1].partner = kPanelGroup;
    panel_[0].down = panelDown_;

    // The file a group shows belongs to it; otherwise the one nearest to it in the tab order.
    auto nearest = [&](int from, int group) {
        from = std::clamp(from, 0, std::max(0, n - 1));
        for (int d = 0; d < n; d++) {
            for (int i : {from - d, from + d}) {
                if (i >= 0 && i < n && editors_[i]->group == group) return i;
            }
        }
        return -1;
    };
    if (active_ < 0 || active_ >= n || editors_[active_]->group != 0) active_ = nearest(active_, 0);
    if (splitIndex_ < 0 || splitIndex_ >= n || editors_[splitIndex_]->group != 1) splitIndex_ = nearest(splitIndex_, 1);
    std::vector<int> groups = {0, 1, kPanelGroup, kPanelGroup2};
    for (auto& [group, fg] : floatGroups_) groups.push_back(group);
    for (int group : groups) {
        if (FloatGroup* info = group >= kPanelGroup ? GroupInfo(group) : nullptr) {
            Editor* shown = FindById(info->shown);
            if (!shown || shown->group != group) {
                int i = nearest(IndexOf(shown), group);
                info->shown = i >= 0 ? editors_[i]->id() : 0;
            }
        }
        // A group without files shows a part; with files, a part only when one was chosen.
        if (ShownPart(group) < 0) {
            std::vector<int> parts = GroupParts(group);
            shownPart_[group] = GroupTabs(group).empty() && !parts.empty() ? parts[0] : -1;
        }
    }
    if (!GroupHasItems(1)) focusRight_ = false;

    // Each file and part in the window of its group, before an emptied window is destroyed with its children.
    for (auto& e : editors_) {
        HWND host = GroupHost(e->group);
        if (host && GetParent(e->hwnd()) != host) {
            ShowWindow(e->hwnd(), SW_HIDE);
            SetParent(e->hwnd(), host);
        }
    }
    for (int part = 0; part < kDebugParts; part++) {
        HWND host = GroupHost(partGroup_[part]);
        HWND input = part == kMemoryPart ? memExpr_ : part == static_cast<int>(DebugTab::Watch) ? watchExpr_ : nullptr;
        for (HWND h : {PartView(part)->hwnd(), input}) {
            if (h && host && GetParent(h) != host) {
                ShowWindow(h, SW_HIDE);
                SetParent(h, host);
            }
        }
    }

    // An empty second half goes away (the window is no longer split); a window without tabs closes.
    std::vector<int> empty;
    for (auto& [group, fg] : floatGroups_) {
        if (!GroupHasItems(group)) empty.push_back(group);
    }
    for (int group : empty) {
        auto it = floatGroups_.find(group);
        if (it == floatGroups_.end()) continue;
        FloatGroup fg = it->second;
        floatGroups_.erase(it);
        shownPart_.erase(group);
        if (floatFocus_ == group) floatFocus_ = fg.second ? fg.partner : 0;
        if (fg.second) {
            if (auto first = floatGroups_.find(fg.partner); first != floatGroups_.end()) first->second.partner = 0;
        } else {
            if (fg.partner) floatGroups_.erase(fg.partner);
            DestroyWindow(fg.hwnd);
        }
    }
    if (floatFocus_ >= kPanelGroup && !GroupHasItems(floatFocus_)) floatFocus_ = 0;
}

void MainWindow::MoveToGroup(int index, int group) {
    if (index < 0 || index >= static_cast<int>(editors_.size())) return;
    Editor* ed = editors_[index].get();
    if (ed->group == group) return;
    ed->group = group;
    Activate(IndexOf(ed));  // shown in its new group (Layout puts everything else in order)
}

void MainWindow::MoveItem(TabItem item, int group) {
    if (item.part >= 0) {
        partGroup_[item.part] = group;
        ActivatePart(item.part);
    } else {
        MoveToGroup(item.editor, group);
    }
}

// Moves a tab into the second half of a window (made when it is not split yet); the first half keeps its
// other tabs.
void MainWindow::SplitItem(TabItem item, int primary, bool down) {
    if (ItemGroup(item) == primary && GroupItemCount(primary) < 2) {
        ShowMessage(hwnd_, Tr(L"Для разделения нужны хотя бы две вкладки: откройте второй файл."), false);
        return;
    }
    if (primary >= kFirstFloatGroup && !floatGroups_.count(primary)) return;
    MoveItem(item, EnsureSecondHalf(primary, down));
}

// Every tab of a window (both halves) into `target`.
void MainWindow::MoveWindowItems(int primary, int target) {
    const FloatGroup* info = GroupInfo(primary);
    Editor* shown = info ? FindById(info->shown) : nullptr;
    int part = ShownPart(primary);
    int second = SecondHalf(primary);
    for (auto& e : editors_) {
        if (e->group == primary || (second && e->group == second)) e->group = target;
    }
    for (int& group : partGroup_) {
        if (group == primary || (second && group == second)) group = target;
    }
    SetForegroundWindow(hwnd_);
    if (part >= 0) ActivatePart(part);
    else if (shown) Activate(IndexOf(shown));
    else Layout();
}

void MainWindow::InvalidateTabs() {
    for (int group : {0, 1, kPanelGroup, kPanelGroup2}) {
        RECT r = StripRect(group);
        InvalidateRect(hwnd_, &r, FALSE);
    }
    for (auto& [group, fg] : floatGroups_) InvalidateRect(fg.hwnd, &fg.rcTabs, FALSE);
}

int MainWindow::TabAt(HWND host, int x, int y, bool* close) const {
    if (close) *close = false;
    for (size_t i = 0; i < editors_.size() && i < tabRects_.size(); i++) {
        int group = editors_[i]->group;
        if (GroupHost(group) != host || !Contains(StripRect(group), x, y) || !Contains(tabRects_[i], x, y)) continue;
        if (close) *close = Contains(closeRects_[i], x, y);
        return static_cast<int>(i);
    }
    return -1;
}

int MainWindow::PartTabAt(HWND host, int x, int y) const {
    for (int part = 0; part < kDebugParts; part++) {
        int group = partGroup_[part];
        if (GroupHost(group) == host && Contains(StripRect(group), x, y) && Contains(partTabRects_[part], x, y)) return part;
    }
    return -1;
}

// ---- windows of their own -----------------------------------------------------------------------------------------

HWND MainWindow::CreateFloat(const std::wstring& title, RECT r) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = FloatProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(1));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kFloatClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    // A window that would be off every monitor (one was unplugged) opens next to the main window instead.
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        RECT main;
        GetWindowRect(hwnd_, &main);
        OffsetRect(&r, main.left + S(60) - r.left, main.top + S(60) - r.top);
    }
    // Not owned by the main window: it has its own taskbar button and can stay in front on another monitor.
    HWND f = CreateWindowExW(WS_EX_APPWINDOW, kFloatClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst_, this);
    if (f) ApplyDarkWindow(f, Theme::FrameBg);
    return f;
}

LRESULT CALLBACK MainWindow::FloatProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    int group = self->FloatGroupOf(hwnd);  // the window's first group
    if (!group) return DefWindowProcW(hwnd, msg, wp, lp);  // being created or destroyed
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    switch (msg) {
    case WM_NOTIFY:  // from the views: handled as if they were in the main window
    case WM_COMMAND:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        return SendMessageW(self->hwnd_, msg, wp, lp);
    case WM_SIZE:
        self->Layout();  // the groups fill the window
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        self->PaintFloat(mem, group);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (self->drag_ == Drag::Tab) {
            self->TabDragMove(hwnd, x, y);
            return 0;
        }
        bool close;
        int tab = self->TabAt(hwnd, x, y, &close);
        int part = self->PartTabAt(hwnd, x, y);
        if (tab != self->hoverTab_ || (close ? tab : -1) != self->hoverClose_ || part != self->hoverPart_) {
            self->hoverTab_ = tab;
            self->hoverClose_ = close ? tab : -1;
            self->hoverPart_ = part;
            self->InvalidateTabs();
        }
        self->MemFormatHover(hwnd, x, y);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self->hoverTab_ >= 0 || self->hoverClose_ >= 0 || self->hoverPart_ >= 0) {
            self->hoverTab_ = self->hoverClose_ = self->hoverPart_ = -1;
            self->InvalidateTabs();
        }
        self->MemFormatHover(hwnd, -1, -1);
        return 0;
    case WM_LBUTTONDOWN:
        if (!self->TabMouseDown(hwnd, x, y) && !self->MemFormatClick(hwnd, x, y)) {
            bool close;
            int tab = self->TabAt(hwnd, x, y, &close);
            if (tab >= 0 && close) self->CloseEditor(tab);
        }
        return 0;
    case WM_LBUTTONUP:
        if (self->drag_ == Drag::Tab) self->TabDrop(hwnd, x, y);
        return 0;
    case WM_CAPTURECHANGED:
        if (self->drag_ == Drag::Tab && reinterpret_cast<HWND>(lp) != hwnd) {  // the drag was cancelled
            self->drag_ = Drag::None;
            self->dragTab_ = self->dragPart_ = -1;
            self->ShowDropHint(nullptr);
            self->InvalidateTabs();
        }
        return 0;
    case WM_MBUTTONUP:
        if (int tab = self->TabAt(hwnd, x, y); tab >= 0 && !self->editors_[tab]->pinned) self->CloseEditor(tab);
        return 0;
    case WM_RBUTTONUP:
        if (int tab = self->TabAt(hwnd, x, y); tab >= 0) self->TabMenu(tab);
        else if (int part = self->PartTabAt(hwnd, x, y); part >= 0) self->PartMenu(part);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{x, y};
        ScreenToClient(hwnd, &p);
        for (int g : {group, self->SecondHalf(group)}) {
            if (!g || !Contains(self->StripRect(g), p.x, p.y)) continue;
            int& scroll = self->tabScroll_[g];
            scroll = std::max(0, scroll - GET_WHEEL_DELTA_WPARAM(wp) / 2);
            self->InvalidateTabs();
            return 0;
        }
        break;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd && self->hoverMemFormat_ &&
            self->PartHost(kMemoryPart) == hwnd) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !IsIconic(hwnd)) {
            // The half that had the focus gets it back.
            int focus = self->WindowPrimary(self->floatFocus_) == group ? self->floatFocus_ : group;
            int part = self->ShownPart(focus);
            Editor* ed = part >= 0 ? self->PartView(part) : self->FindById(self->floatGroups_[focus].shown);
            if (ed) {
                self->floatFocus_ = focus;
                SetFocus(ed->hwnd());
                return 0;
            }
        }
        break;
    case WM_ENTERSIZEMOVE:
        self->floatMoved_ = false;
        break;
    case WM_MOVING:  // only while moved, not resized
        self->floatMoved_ = true;
        self->FloatMoving(hwnd, false);
        break;
    case WM_EXITSIZEMOVE:
        if (self->floatMoved_) self->FloatMoving(hwnd, true);
        self->floatMoved_ = false;
        break;
    case WM_FLOAT_DOCK: {  // every tab of the window joins the group it was dropped on
        POINT at{static_cast<LONG>(wp), static_cast<LONG>(lp)};
        RECT hint;
        int target = 0;
        if (self->DockTargetAt(hwnd, at, hint, target)) self->MoveWindowItems(group, target);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {self->S(260), self->S(200)};
        return 0;
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: {  // the files go back to the main editor, the parts of the debugger to the debug panel
        int second = self->SecondHalf(group);
        const FloatGroup* info = self->GroupInfo(group);
        Editor* shown = info ? self->FindById(info->shown) : nullptr;
        for (auto& e : self->editors_) {
            if (e->group == group || (second && e->group == second)) e->group = 0;
        }
        for (int& g : self->partGroup_) {
            if (g == group || (second && g == second)) g = kPanelGroup;
        }
        SetForegroundWindow(self->hwnd_);
        if (shown) self->Activate(self->IndexOf(shown));
        else self->Layout();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void MainWindow::UpdateFloatTitles() {
    for (auto& [group, fg] : floatGroups_) {
        if (fg.second) continue;
        // The title names the tab shown in the half that has the focus.
        int focus = WindowPrimary(floatFocus_) == group ? floatFocus_ : group;
        std::wstring title;
        if (int part = ShownPart(focus); part >= 0) {
            title = PartTitle(part);
        } else if (Editor* ed = FindById(floatGroups_.at(focus).shown)) {
            title = (ed->Modified() ? L"● " : L"") + ed->Title();
        }
        SetWindowTextW(fg.hwnd, (title + L" – AsmEditor").c_str());
    }
}

RECT MainWindow::DetachedRect(const POINT* at) const {
    // As large as the editor area of the main window; under the cursor, or a little below the editor.
    RECT area = GroupArea(0);
    if (GroupHasItems(1)) area = Union(area, GroupArea(1));
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&area), 2);
    int w = std::max<int>(S(480), area.right - area.left), h = std::max<int>(S(360), area.bottom - area.top);
    POINT origin = at ? POINT{at->x - S(80), at->y - S(14)} : POINT{area.left + S(40), area.top + S(40)};
    return {origin.x, origin.y, origin.x + w, origin.y + h};
}

// A tab in a new window of its own.
void MainWindow::DetachItem(TabItem item, const POINT* at) {
    if (item.part < 0 && (item.editor < 0 || item.editor >= static_cast<int>(editors_.size()))) return;
    std::wstring title = item.part >= 0 ? PartTitle(item.part) : editors_[item.editor]->Title();
    HWND f = CreateFloat(title + L" – AsmEditor", DetachedRect(at));
    if (!f) return;
    int group = nextGroup_++;
    floatGroups_[group].hwnd = f;
    if (item.part >= 0 && !DebugPanelVisible()) {  // a window of only parts shows while the panel would
        showDebugPanel_ = true;
        CheckMenuItem(viewMenu_, ID_DEBUG_PANEL, MF_CHECKED);
    }
    // The tab joins the window before it shows: a window without tabs is closed by the next Layout.
    if (item.part >= 0) {
        partGroup_[item.part] = group;
    } else {
        editors_[item.editor]->group = group;
        floatGroups_[group].shown = editors_[item.editor]->id();
    }
    ShowWindow(f, SW_SHOW);
    if (item.part >= 0) ActivatePart(item.part);
    else Activate(item.editor);
    UpdateTitle();
}

void MainWindow::DetachEditor(int index, const POINT* at) {
    DetachItem({index, -1}, at);
}

void MainWindow::DockEditor(Editor* ed) {
    if (!IsFloating(ed)) return;
    SetForegroundWindow(hwnd_);
    MoveToGroup(IndexOf(ed), 0);
}

// While a window of its own is dragged by its title: over the main window it shows where it docks, and docks
// when it is dropped there.
void MainWindow::FloatMoving(HWND f, bool done) {
    POINT c;
    GetCursorPos(&c);
    RECT hint;
    int group = 0;
    bool over = DockTargetAt(f, c, hint, group);
    ShowDropHint(over && !done ? &hint : nullptr);
    if (over && done) PostMessageW(f, WM_FLOAT_DOCK, static_cast<WPARAM>(c.x), static_cast<LPARAM>(c.y));
}

// Where the debug panel would be while it is hidden (client): parts dropped there bring it back.
RECT MainWindow::PanelDropStrip() const {
    if (PanelShown()) return {};
    RECT body{rcToolbar_.left, rcToolbar_.bottom, rcStatus_.right, rcStatus_.top};
    int w = std::clamp(S(settings_->debugWidth), S(220), std::max(S(220), (int)(body.right - body.left) / 3));
    return {body.right - w, body.top, body.right, body.bottom};
}

// Where a window dragged to `screen` docks in the main one: a group of the debug panel or of the editor (a window
// of only parts: also where the hidden panel would be). hint: the area to highlight (screen).
bool MainWindow::DockTargetAt(HWND f, POINT screen, RECT& hint, int& group) const {
    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return false;
    POINT c = screen;
    ScreenToClient(hwnd_, &c);
    int primary = FloatGroupOf(f), second = SecondHalf(primary);
    bool files = false;
    for (auto& e : editors_) files |= e->group == primary || (second && e->group == second);
    RECT strip = PanelDropStrip();
    if (PanelShown()) {
        for (int g : {kPanelGroup, kPanelGroup2}) {
            RECT area = GroupArea(g);
            if (GroupHasItems(g) && PtInRect(&area, c)) {
                group = g;
                hint = area;
                MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&hint), 2);
                return true;
            }
        }
    } else if (!files && PtInRect(&strip, c)) {
        group = kPanelGroup;
        hint = strip;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&hint), 2);
        return true;
    }
    RECT half = GroupArea(1);
    group = GroupHasItems(1) && PtInRect(&half, c) ? 1 : 0;
    hint = GroupArea(group);
    if (!PtInRect(&hint, c)) return false;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&hint), 2);
    return true;
}

// ---- tab drag & drop ----------------------------------------------------------------------------------------------

bool MainWindow::TabMouseDown(HWND host, int x, int y) {
    bool close;
    int tab = TabAt(host, x, y, &close);
    int part = tab < 0 ? PartTabAt(host, x, y) : -1;
    if ((tab < 0 || close) && part < 0) return false;
    if (tab >= 0) Activate(tab);
    else ActivatePart(part);
    drag_ = Drag::Tab;
    dragTab_ = tab;
    dragPart_ = part;
    dragStartX_ = x;
    dragStartY_ = y;
    dragMoved_ = false;
    dragHost_ = host;
    SetCapture(host);
    return true;
}

void MainWindow::TabDragMove(HWND host, int x, int y) {
    bool isPart = dragPart_ >= 0;
    if (!isPart && (dragTab_ < 0 || dragTab_ >= static_cast<int>(editors_.size()))) return;
    if (!dragMoved_ && std::abs(x - dragStartX_) < S(8) && std::abs(y - dragStartY_) < S(8)) return;
    dragMoved_ = true;
    int group = isPart ? partGroup_[dragPart_] : editors_[dragTab_]->group;
    if (Contains(StripRect(group), x, y)) {  // along its own tab row: files change places
        ShowDropHint(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        for (int i : isPart ? std::vector<int>{} : GroupTabs(group)) {
            if (i == dragTab_ || x < tabRects_[i].left || x >= tabRects_[i].right) continue;
            if (editors_[i]->pinned != editors_[dragTab_]->pinned) break;  // pinned tabs stay in front
            MoveTab(dragTab_, i);
            dragTab_ = i;
            break;
        }
        InvalidateTabs();
        return;
    }
    POINT screen{x, y};
    ClientToScreen(host, &screen);
    RECT hint;
    int target = 0;
    Drop zone = TabDropZone(screen, hint, target);
    ShowDropHint(zone == Drop::None ? nullptr : &hint);
    SetCursor(LoadCursorW(nullptr, zone == Drop::None ? IDC_NO : IDC_ARROW));
}

void MainWindow::TabDrop(HWND host, int x, int y) {
    TabItem item{dragTab_, dragPart_};
    bool valid = item.part >= 0 || (item.editor >= 0 && item.editor < static_cast<int>(editors_.size()));
    POINT screen{x, y};
    ClientToScreen(host, &screen);
    RECT hint;
    int target = 0;
    Drop zone = valid && dragMoved_ && !Contains(StripRect(ItemGroup(item)), x, y) ? TabDropZone(screen, hint, target)
                                                                                  : Drop::None;
    drag_ = Drag::None;
    dragTab_ = dragPart_ = -1;
    ReleaseCapture();
    ShowDropHint(nullptr);
    InvalidateTabs();
    switch (zone) {
    case Drop::Right: SplitItem(item, target, false); break;
    case Drop::Down: SplitItem(item, target, true); break;
    case Drop::Window: DetachItem(item, &screen); break;
    case Drop::Group: MoveItem(item, target); break;
    case Drop::None: break;
    }
}

// Where a tab dropped at `client` of the window whose first group is `primary` goes: into the other half of a
// split window, else a split (the right or lower third of the editor) or into the group.
MainWindow::Drop MainWindow::ZoneIn(int primary, POINT c, int from, RECT& hint, int& group) const {
    if (int second = SecondHalf(primary)) {
        for (int g : {primary, second}) {
            RECT area = GroupArea(g);
            if (!PtInRect(&area, c)) continue;
            if (g == from) return Drop::None;
            group = g;
            hint = GroupContent(g);
            return Drop::Group;
        }
        return Drop::None;
    }
    RECT area = GroupArea(primary), ed = GroupContent(primary);
    if (!PtInRect(&area, c)) return Drop::None;
    int w = ed.right - ed.left, h = ed.bottom - ed.top;
    bool canSplit = from != primary || GroupItemCount(primary) > 1;  // the first half keeps a tab
    group = primary;
    if (canSplit && PtInRect(&ed, c) && c.x >= ed.left + w * 2 / 3) {
        hint = {ed.left + w / 2, ed.top, ed.right, ed.bottom};
        return Drop::Right;
    }
    if (canSplit && PtInRect(&ed, c) && c.y >= ed.top + h * 2 / 3) {
        hint = {ed.left, ed.top + h / 2, ed.right, ed.bottom};
        return Drop::Down;
    }
    if (from == primary) return Drop::None;
    hint = ed;
    return Drop::Group;
}

// Where a dragged tab goes when dropped at `screen`: a group of the main window (the debug panel or the editor)
// or of a window of its own, or, outside every window, a new window of its own.
MainWindow::Drop MainWindow::TabDropZone(POINT screen, RECT& hint, int& group) const {
    int from = dragPart_ >= 0 ? partGroup_[dragPart_] : editors_[dragTab_]->group;
    HWND root = GetAncestor(WindowFromPoint(screen), GA_ROOT);
    int primary = root == hwnd_ ? -1 : FloatGroupOf(root);
    if (root == hwnd_ || primary) {
        POINT c = screen;
        ScreenToClient(root, &c);
        Drop zone = Drop::None;
        if (root == hwnd_) {
            if (PanelShown()) zone = ZoneIn(kPanelGroup, c, from, hint, group);
            RECT strip = PanelDropStrip();
            if (zone == Drop::None && dragPart_ >= 0 && PtInRect(&strip, c)) {  // a part brings the panel back
                zone = Drop::Group;
                group = kPanelGroup;
                hint = strip;
            }
            if (zone == Drop::None) zone = ZoneIn(0, c, from, hint, group);
        } else {
            zone = ZoneIn(primary, c, from, hint, group);
        }
        if (zone != Drop::None) MapWindowPoints(root, nullptr, reinterpret_cast<POINT*>(&hint), 2);
        return zone;
    }
    // The only tab of a window: moving the window itself does that.
    if (from >= kFirstFloatGroup && GroupItemCount(from) < 2 && !SecondHalf(WindowPrimary(from))) return Drop::None;
    hint = DetachedRect(&screen);
    return Drop::Window;
}

void MainWindow::ShowDropHint(const RECT* r) {
    if (!r) {
        if (dropHint_) ShowWindow(dropHint_, SW_HIDE);
        return;
    }
    if (!dropHint_) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DropHintProc;
        wc.hInstance = inst_;
        wc.lpszClassName = kDropHintClass;
        RegisterClassW(&wc);
        dropHint_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                    kDropHintClass, L"", WS_POPUP, 0, 0, 0, 0, hwnd_, nullptr, inst_, nullptr);
        SetLayeredWindowAttributes(dropHint_, 0, 70, LWA_ALPHA);
    }
    SetWindowPos(dropHint_, HWND_TOPMOST, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(dropHint_, nullptr, TRUE);
}

// ---- parts of the debugger ----------------------------------------------------------------------------------------

Editor* MainWindow::PartView(int part) const {
    switch (part) {
    case 0: return regsView_.get();
    case 1: return stackView_.get();
    case 2: return watchView_.get();
    case 3: return disasmView_.get();
    case 4: return flagsView_.get();
    default: return memView_.get();
    }
}

std::wstring MainWindow::PartTitle(int part) const {
    static const wchar_t* titles[] = {L"Регистры", L"Стек вызовов", L"Наблюдение", L"Код", L"Флаги", L"Память"};
    return Tr(titles[std::clamp(part, 0, kMemoryPart)]);
}

// Shows a part in its group (and that group's window), the debug panel included when it was hidden.
void MainWindow::ActivatePart(int part, bool focus) {
    if (part < 0 || part >= kDebugParts) return;
    int group = partGroup_[part];
    shownPart_[group] = part;
    if (!DebugPanelVisible() && !WindowShown(WindowPrimary(group))) {
        showDebugPanel_ = true;
        CheckMenuItem(viewMenu_, ID_DEBUG_PANEL, MF_CHECKED);
    }
    ensureTabVisible_.insert(group);
    Layout();
    HWND host = GroupHost(group);
    if (host && host != hwnd_) {
        if (IsIconic(host)) ShowWindow(host, SW_RESTORE);
        if (focus) SetForegroundWindow(host);
        else SetWindowPos(host, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (focus) {
        SetFocus(part == static_cast<int>(DebugTab::Watch) ? watchExpr_ : PartView(part)->hwnd());
        floatFocus_ = group >= kPanelGroup ? group : 0;
    }
    if (part == static_cast<int>(DebugTab::Disasm)) RenderDisassembly();
    if (part == static_cast<int>(DebugTab::Watch)) RenderWatches();
    UpdateFloatTitles();
    InvalidateTabs();
}

// Right click on a part's tab.
void MainWindow::PartMenu(int part) {
    enum { kRight = 1, kDown, kUnsplit, kWindow, kPanel };
    int group = partGroup_[part], primary = WindowPrimary(group);
    HMENU menu = CreatePopupMenu();
    if (!IsSecondHalf(group)) {
        AppendMenuW(menu, MF_STRING, kRight, Tr(L"Открыть справа"));
        AppendMenuW(menu, MF_STRING, kDown, Tr(L"Открыть снизу"));
    }
    if (SecondHalf(primary)) AppendMenuW(menu, MF_STRING, kUnsplit, Tr(L"Убрать разделение"));
    AppendMenuW(menu, MF_STRING, kWindow, Tr(L"Открыть в отдельном окне"));
    if (group != kPanelGroup && group != kPanelGroup2) AppendMenuW(menu, MF_STRING, kPanel, Tr(L"Вернуть в панель отладки"));
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, GroupHost(group), nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case kRight: SplitItem({-1, part}, primary, false); break;
    case kDown: SplitItem({-1, part}, primary, true); break;
    case kUnsplit: Unsplit(primary); break;
    case kWindow: DetachItem({-1, part}, nullptr); break;
    case kPanel: MoveItem({-1, part}, kPanelGroup); break;
    }
}

// The parts back where they are at first: the debugger's tabs above the memory view in the panel.
void MainWindow::ResetDebugParts() {
    for (int part = 0; part < kDebugParts; part++) partGroup_[part] = part == kMemoryPart ? kPanelGroup2 : kPanelGroup;
    shownPart_[kPanelGroup] = 0;
    shownPart_[kPanelGroup2] = kMemoryPart;
    panelDown_ = true;
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// View → the debug panel in a window of its own (split as it is), or the parts back into the panel.
void MainWindow::ToggleDebugWindow() {
    if (!GroupHasItems(kPanelGroup)) {
        ResetDebugParts();
        return;
    }
    RECT r = rcDebug_;
    if (r.right > r.left) {
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
        OffsetRect(&r, -S(40), S(30));
    } else {
        r = DetachedRect(nullptr);
    }
    HWND f = CreateFloat(std::wstring(Tr(L"Отладка")) + L" – AsmEditor", r);
    if (!f) return;
    int primary = nextGroup_++;
    floatGroups_[primary].hwnd = f;
    int second = GroupHasItems(kPanelGroup2) ? EnsureSecondHalf(primary, panelDown_) : 0;
    int shown1 = ShownPart(kPanelGroup), shown2 = ShownPart(kPanelGroup2);
    floatGroups_[primary].shown = panel_[0].shown;
    if (second) floatGroups_[second].shown = panel_[1].shown;
    for (auto& e : editors_) {
        if (e->group == kPanelGroup) e->group = primary;
        if (e->group == kPanelGroup2) e->group = second;
    }
    for (int& group : partGroup_) {
        if (group == kPanelGroup) group = primary;
        else if (group == kPanelGroup2) group = second;
    }
    shownPart_[primary] = shown1;
    if (second) shownPart_[second] = shown2;
    if (!DebugPanelVisible()) {
        showDebugPanel_ = true;
        CheckMenuItem(viewMenu_, ID_DEBUG_PANEL, MF_CHECKED);
    }
    Layout();
    ShowWindow(f, SW_SHOW);
    UpdateFloatTitles();
}

// Settings: where each part is ("P<part>=<group 0..3>" or "=w<window>.<half>") and the windows of their own with
// parts ("W<window>=x,y,width,height,down").
void MainWindow::SaveDebugLayout() {
    std::map<int, int> windows;  // first group -> number
    std::wstring text = L"D=" + std::to_wstring(panelDown_ ? 1 : 0) + L";";
    for (int part = 0; part < kDebugParts; part++) {
        int group = partGroup_[part];
        std::wstring where = std::to_wstring(group);
        if (group >= kFirstFloatGroup) {
            int primary = WindowPrimary(group);
            if (!windows.count(primary)) {
                int k = static_cast<int>(windows.size());
                windows[primary] = k;
                const FloatGroup& fg = floatGroups_.at(primary);
                RECT r;
                if (IsIconic(fg.hwnd)) {
                    WINDOWPLACEMENT wpl{sizeof(wpl)};
                    GetWindowPlacement(fg.hwnd, &wpl);
                    r = wpl.rcNormalPosition;
                } else {
                    GetWindowRect(fg.hwnd, &r);
                }
                text += L"W" + std::to_wstring(k) + L"=" + std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," +
                        std::to_wstring(r.right - r.left) + L"," + std::to_wstring(r.bottom - r.top) + L"," +
                        (fg.down ? L"1" : L"0") + L";";
            }
            where = L"w" + std::to_wstring(windows[primary]) + L"." + (group == primary ? L"0" : L"1");
        }
        text += L"P" + std::to_wstring(part) + L"=" + where + L";";
    }
    settings_->debugParts = text;
    settings_->Save();
}

void MainWindow::RestoreDebugLayout() {
    std::map<int, int> windows;     // number -> first group
    std::map<int, bool> downs;
    std::vector<std::pair<int, std::wstring>> parts;
    for (const std::wstring& item : Split(settings_->debugParts, L';')) {
        size_t eq = item.find(L'=');
        if (eq == std::wstring::npos || eq < 1) continue;
        std::wstring key = item.substr(0, eq), value = item.substr(eq + 1);
        if (key == L"D") panelDown_ = value != L"0";
        else if (key[0] == L'P') parts.push_back({_wtoi(key.c_str() + 1), value});
        else if (key[0] == L'W') {
            int v[5] = {};
            if (swscanf_s(value.c_str(), L"%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5) continue;
            if (v[2] < S(150) || v[3] < S(100)) continue;
            HWND f = CreateFloat(L"AsmEditor", {v[0], v[1], v[0] + v[2], v[1] + v[3]});
            if (!f) continue;
            int primary = nextGroup_++;
            floatGroups_[primary].hwnd = f;
            windows[_wtoi(key.c_str() + 1)] = primary;
            downs[primary] = v[4] != 0;
        }
    }
    for (auto& [part, where] : parts) {
        if (part < 0 || part >= kDebugParts) continue;
        if (where[0] == L'w') {
            int k = 0, half = 0;
            if (swscanf_s(where.c_str() + 1, L"%d.%d", &k, &half) != 2 || !windows.count(k)) continue;
            int primary = windows[k];
            partGroup_[part] = half ? EnsureSecondHalf(primary, downs[primary]) : primary;
        } else {
            int group = _wtoi(where.c_str());
            partGroup_[part] = group >= 0 && group < kFirstFloatGroup ? group : kPanelGroup;
        }
    }
    Layout();  // a window left without parts (a part missing from the settings) closes here
    UpdateFloatTitles();
}
