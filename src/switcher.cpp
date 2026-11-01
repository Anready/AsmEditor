#include "switcher.h"

#include "fileicons.h"

namespace {

struct Popup {
    const std::vector<Switcher::Item>* items = nullptr;
    std::wstring title;
    int selected = 0;
    int dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    int S(int v) const { return MulDiv(v, dpi, 96); }
    int RowHeight() const { return S(28); }
    int Top() const { return S(40); }
};

void Paint(Popup& p, HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    auto fill = [&](RECT r, COLORREF c) {
        SetDCBrushColor(mem, c);
        FillRect(mem, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    };
    fill(rc, Theme::PanelBg);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, p.bold);
    SetTextColor(mem, Theme::Muted);
    RECT title{p.S(16), 0, rc.right - p.S(16), p.Top()};
    DrawTextW(mem, p.title.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(mem, p.font);
    int icon = p.S(16);
    for (size_t i = 0; i < p.items->size(); i++) {
        const Switcher::Item& item = (*p.items)[i];
        RECT r{p.S(8), p.Top() + static_cast<int>(i) * p.RowHeight(), rc.right - p.S(8),
               p.Top() + static_cast<int>(i + 1) * p.RowHeight()};
        if (static_cast<int>(i) == p.selected) {
            SetDCBrushColor(mem, Theme::ListSelected);
            SelectObject(mem, GetStockObject(DC_BRUSH));
            SelectObject(mem, GetStockObject(NULL_PEN));
            RoundRect(mem, r.left, r.top, r.right + 1, r.bottom + 1, p.S(8), p.S(8));
        }
        int x = r.left + p.S(8);
        if (item.icon >= 0) FileIcons::Draw(mem, x, (r.top + r.bottom - icon) / 2, icon, static_cast<FileIcons::Kind>(item.icon));
        x += icon + p.S(8);
        SIZE sz{};
        GetTextExtentPoint32W(mem, item.name.c_str(), static_cast<int>(item.name.size()), &sz);
        RECT nr{x, r.top, r.right - p.S(8), r.bottom};
        SetTextColor(mem, static_cast<int>(i) == p.selected ? Theme::Bright : Theme::Text);
        DrawTextW(mem, item.name.c_str(), -1, &nr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT dr{x + sz.cx + p.S(10), r.top, r.right - p.S(8), r.bottom};
        SetTextColor(mem, Theme::Muted);
        DrawTextW(mem, item.detail.c_str(), -1, &dr, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(mem, oldFont);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<Popup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
        if (p) Paint(*p, hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

namespace Switcher {

std::optional<int> Run(HWND owner, const std::wstring& title, const std::vector<Item>& items, int start) {
    if (items.empty()) return std::nullopt;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AsmSwitcher";
        wc.style = CS_DROPSHADOW;
        RegisterClassW(&wc);
        registered = true;
    }
    Popup p;
    p.items = &items;
    p.title = title;
    p.selected = std::clamp(start, 0, static_cast<int>(items.size()) - 1);
    p.dpi = static_cast<int>(GetDpiForWindow(owner));
    if (!p.dpi) p.dpi = 96;
    p.font = CreateUiFont(p.dpi, 9);
    p.bold = CreateUiFont(p.dpi, 9, true);
    int rows = std::min<int>(static_cast<int>(items.size()), 16);
    int w = p.S(480), h = p.Top() + rows * p.RowHeight() + p.S(10);
    RECT orc;
    GetWindowRect(owner, &orc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"AsmSwitcher", L"", WS_POPUP | WS_BORDER,
                                (orc.left + orc.right - w) / 2, orc.top + (orc.bottom - orc.top) / 4, w, h, owner,
                                nullptr, GetModuleHandleW(nullptr), &p);
    ApplyDarkWindow(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    // Started with Ctrl down (Ctrl+Tab): releasing it picks. From a menu: Enter or a click does.
    bool holding = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    std::optional<int> result;
    bool done = false;
    auto move = [&](int by) {
        int n = static_cast<int>(items.size());
        p.selected = ((p.selected + by) % n + n) % n;
        InvalidateRect(hwnd, nullptr, FALSE);
    };
    MSG msg;
    while (!done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        switch (msg.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg.wParam == VK_TAB) move((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            else if (msg.wParam == VK_DOWN) move(1);
            else if (msg.wParam == VK_UP) move(-1);
            else if (msg.wParam == VK_RETURN) result = p.selected, done = true;
            else if (msg.wParam == VK_ESCAPE) done = true;
            continue;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (msg.wParam == VK_CONTROL && holding) result = p.selected, done = true;
            continue;
        case WM_CHAR:
        case WM_SYSCHAR:
            continue;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
            if (msg.hwnd == hwnd) {
                int y = GET_Y_LPARAM(msg.lParam) - p.Top();
                if (msg.message == WM_LBUTTONDOWN && y >= 0 && y / p.RowHeight() < static_cast<int>(items.size())) {
                    result = y / p.RowHeight();
                }
            }
            done = true;  // a click elsewhere cancels
            continue;
        }
        if (msg.message == WM_ACTIVATEAPP || (msg.message == WM_MOUSEMOVE && msg.hwnd == hwnd)) {
            if (msg.message == WM_MOUSEMOVE) {
                int y = GET_Y_LPARAM(msg.lParam) - p.Top();
                int row = y >= 0 ? y / p.RowHeight() : -1;
                if (row >= 0 && row < static_cast<int>(items.size()) && row != p.selected) {
                    p.selected = row;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        DWORD pid = 0;  // another program came to the front
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid != GetCurrentProcessId()) done = true;
    }
    DestroyWindow(hwnd);
    DeleteObject(p.font);
    DeleteObject(p.bold);
    return result;
}
}
