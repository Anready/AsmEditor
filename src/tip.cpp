#include "tip.h"

#include <dwmapi.h>

namespace {

constexpr wchar_t kTipClass[] = L"AsmEditorTip";

// What one line of the card looks like.
struct Line {
    std::wstring text;
    bool header = false;  // the highlighted part: code font, link color
    bool code = false;    // aligned text (columns, tables of values): code font
    RECT rc{};            // where it is drawn (client)
};

struct State {
    HWND hwnd = nullptr;
    std::vector<Line> lines;
    int dpi = 96;
    bool roundedByDwm = false;  // Windows 11 rounds the window and draws its border
    RECT separator{};           // the line under the header (empty: none)
    ULONGLONG shownAt = 0;
    HFONT ui = nullptr, code = nullptr, codeBold = nullptr;
} g;

int S(int v) {
    return MulDiv(v, g.dpi, 96);
}

void MakeFonts(int dpi) {
    if (g.ui && dpi == g.dpi) return;
    for (HFONT f : {g.ui, g.code, g.codeBold}) {
        if (f) DeleteObject(f);
    }
    g.dpi = dpi;
    std::wstring mono = MonospaceFontName();
    auto font = [&](const wchar_t* face, int points10, int weight) {
        return CreateFontW(-MulDiv(points10, dpi, 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g.ui = font(L"Segoe UI", 95, FW_NORMAL);
    g.code = font(mono.c_str(), 95, FW_NORMAL);
    g.codeBold = font(mono.c_str(), 95, FW_SEMIBOLD);
}

HFONT FontOf(const Line& l) {
    return l.header ? g.codeBold : l.code ? g.code : g.ui;
}

void Paint(HDC dc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(Theme::CallTipBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    if (!g.roundedByDwm) {  // a plain border where the system does not draw one
        HBRUSH border = CreateSolidBrush(Theme::Border);
        FrameRect(dc, &rc, border);
        DeleteObject(border);
    }
    SetBkMode(dc, TRANSPARENT);
    for (const Line& l : g.lines) {
        if (l.text.empty()) continue;
        SelectObject(dc, FontOf(l));
        SetTextColor(dc, l.header ? Theme::Link : (l.code ? Theme::Bright : Theme::Text));
        RECT r = l.rc;
        DrawTextW(dc, l.text.c_str(), static_cast<int>(l.text.size()), &r,
                  DT_NOPREFIX | DT_EXPANDTABS | (l.code || l.header ? DT_SINGLELINE : DT_WORDBREAK));
    }
    if (g.separator.right > g.separator.left) {
        HBRUSH line = CreateSolidBrush(Theme::Blend(Theme::CallTipBg, Theme::Border, 80));
        FillRect(dc, &g.separator, line);
        DeleteObject(line);
    }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:  // the mouse goes through to the editor
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
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
        Paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND Window() {
    if (g.hwnd) return g.hwnd;
    WNDCLASSW wc{};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTipClass;
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTipClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                             nullptr, wc.hInstance, nullptr);
    // Windows 11: rounded corners and a thin border of the theme's color.
    DWORD round = 3;  // DWMWCP_ROUNDSMALL
    g.roundedByDwm = SUCCEEDED(DwmSetWindowAttribute(g.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round)));
    return g.hwnd;
}

// Splits the text into lines: those overlapping [hltFrom, hltTo) are the header, a line with columns (two
// spaces in a row or a tab inside it) is code, the rest is prose that wraps.
std::vector<Line> Lines(const std::string& utf8, size_t hltFrom, size_t hltTo) {
    std::vector<Line> out;
    size_t start = 0;
    while (start <= utf8.size()) {
        size_t end = utf8.find('\n', start);
        if (end == std::string::npos) end = utf8.size();
        std::string text = utf8.substr(start, end - start);
        if (!text.empty() && text.back() == '\r') text.pop_back();
        Line l;
        l.text = Widen(text);
        l.header = hltTo > hltFrom && start < hltTo && end > hltFrom;
        size_t inner = text.find_first_not_of(" \t");
        l.code = !l.header && inner != std::string::npos &&
                 (inner > 0 || text.find("  ", inner) != std::string::npos || text.find('\t', inner) != std::string::npos);
        out.push_back(std::move(l));
        if (end == utf8.size()) break;
        start = end + 1;
    }
    while (!out.empty() && out.back().text.empty()) out.pop_back();
    return out;
}

}  // namespace

namespace Tip {

void Show(HWND editor, int x, int lineTop, int lineBottom, const std::string& utf8, size_t hltFrom, size_t hltTo) {
    HWND owner = GetAncestor(editor, GA_ROOT);
    HWND w = Window();
    MakeFonts(GetDpiForWindow(editor));
    g.lines = Lines(utf8, hltFrom, hltTo);
    if (g.lines.empty()) {
        Hide();
        return;
    }
    // Measure: code lines keep their width, prose wraps at a comfortable reading width.
    int padX = S(12), padY = S(9), maxProse = S(520), gap = S(2);
    HDC dc = GetDC(w);
    int width = 0;
    std::vector<SIZE> sizes;
    for (const Line& l : g.lines) {
        SelectObject(dc, FontOf(l));
        RECT r{0, 0, maxProse, 0};
        std::wstring text = l.text.empty() ? L" " : l.text;
        if (l.code || l.header) {
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | DT_EXPANDTABS);
        } else {
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
        }
        if (l.text.empty()) r.bottom = r.bottom * 3 / 5;  // an empty line: a smaller gap
        sizes.push_back({r.right - r.left, r.bottom - r.top});
        width = std::max<int>(width, r.right - r.left);
    }
    ReleaseDC(w, dc);
    int y = padY;
    g.separator = {};
    for (size_t i = 0; i < g.lines.size(); i++) {
        Line& l = g.lines[i];
        l.rc = {padX, y, padX + width, y + sizes[i].cy};
        y += sizes[i].cy + gap;
        // Under the header, before the rest: a separator, as between the signature and the text in IntelliJ.
        bool lastHeader = l.header && (i + 1 == g.lines.size() || !g.lines[i + 1].header);
        if (lastHeader && i + 1 < g.lines.size()) {
            y += S(5);
            g.separator = {0, y, padX * 2 + width, y + 1};
            y += S(6);
            while (i + 1 < g.lines.size() && g.lines[i + 1].text.empty()) {  // no blank line right after it
                g.lines[++i].rc = {};
            }
        }
    }
    int cw = padX * 2 + width, ch = y - gap + padY;

    // Above the line, where IntelliJ puts it; below when the screen ends there.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({x, lineTop}, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int left = std::clamp(x - padX, static_cast<int>(wa.left) + S(4), std::max<int>(wa.left + S(4), wa.right - cw - S(4)));
    int top = lineTop - ch - S(4);
    if (top < wa.top) top = lineBottom + S(4);
    if (g.roundedByDwm) {
        COLORREF border = Theme::Border;
        DwmSetWindowAttribute(w, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    }
    SetWindowLongPtrW(w, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));  // above the window it hints in
    SetWindowPos(w, HWND_TOP, left, top, cw, ch, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(w, nullptr, FALSE);
    g.shownAt = GetTickCount64();
}

void Hide() {
    if (g.hwnd && IsWindowVisible(g.hwnd)) ShowWindow(g.hwnd, SW_HIDE);
}

void HideUnlessNew() {
    if (GetTickCount64() - g.shownAt > 300) Hide();
}

bool Visible() {
    return g.hwnd && IsWindowVisible(g.hwnd);
}

HWND Handle() {
    return g.hwnd;
}

}  // namespace Tip
