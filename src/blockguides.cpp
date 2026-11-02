#include "blockguides.h"

#include "editor.h"
#include "scrollbar.h"

#include <set>

namespace {

constexpr UINT_PTR kSubclassId = 0x5CD0;

void Draw(Editor& ed) {
    HWND hwnd = ed.hwnd();
    Sci_Position first = ed.Call(SCI_GETFIRSTVISIBLELINE);
    Sci_Position count = ed.Call(SCI_GETLINECOUNT);
    Sci_Position docFirst = ed.Call(SCI_DOCLINEFROMVISIBLE, first);
    Sci_Position docLast = std::min(count - 1, ed.Call(SCI_DOCLINEFROMVISIBLE, first + ed.Call(SCI_LINESONSCREEN) + 1));
    if (docFirst > docLast) return;

    // The blocks on screen: those the top line is in, and those that open on screen.
    std::set<Sci_Position> headers;
    for (Sci_Position p = docFirst; (p = ed.Call(SCI_GETFOLDPARENT, p)) >= 0 && headers.size() < 32;) headers.insert(p);
    for (Sci_Position l = docFirst; l <= docLast; l++) {
        if (ed.Call(SCI_GETFOLDLEVEL, l) & SC_FOLDLEVELHEADERFLAG) headers.insert(l);
    }
    if (headers.empty()) return;
    Sci_Position caretLine = ed.Call(SCI_LINEFROMPOSITION, ed.Call(SCI_GETCURRENTPOS));
    Sci_Position current = ed.Call(SCI_GETFOLDPARENT, caretLine);

    int textLeft = static_cast<int>(ed.Call(SCI_GETMARGINLEFT));
    for (int m = 0, n = static_cast<int>(ed.Call(SCI_GETMARGINS)); m < n; m++) {
        textLeft += static_cast<int>(ed.Call(SCI_GETMARGINWIDTHN, m));
    }
    int charW = static_cast<int>(ed.Call(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>("x")));
    int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    int width = std::max(1, MulDiv(1, dpi ? dpi : 96, 96));
    int shift = ScrollBars::TopOffset(hwnd);  // the text is drawn higher while it scrolls smoothly
    COLORREF dim = Theme::Blend(Theme::EditorBg, Theme::Muted, 22), bright = Theme::Blend(Theme::EditorBg, Theme::Muted, 60);

    HDC dc = GetDC(hwnd);
    for (Sci_Position h : headers) {
        if (!ed.Call(SCI_GETFOLDEXPANDED, h)) continue;
        Sci_Position end = std::min(docLast, ed.Call(SCI_GETLASTCHILD, h, -1));
        if (end <= h || end < docFirst) continue;
        int indent = static_cast<int>(ed.Call(SCI_GETLINEINDENTATION, h));
        int x = static_cast<int>(ed.Call(SCI_POINTXFROMPOSITION, 0, ed.Call(SCI_GETLINEINDENTPOSITION, h)));
        if (h < docFirst) {  // off screen above: its column from the indentation
            x = textLeft - static_cast<int>(ed.Call(SCI_GETXOFFSET)) + indent * charW;
        }
        x += charW / 2;
        if (x < textLeft) continue;
        SetDCBrushColor(dc, h == current ? bright : dim);
        auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        int top = -1, bottom = -1;
        auto flush = [&] {
            if (top >= 0 && bottom > top) {
                RECT r{x, top, x + width, bottom};
                FillRect(dc, &r, brush);
            }
            top = bottom = -1;
        };
        for (Sci_Position l = std::max(h + 1, docFirst); l <= end; l++) {
            if (!ed.Call(SCI_GETLINEVISIBLE, l)) continue;
            bool blank = ed.Call(SCI_GETLINEINDENTPOSITION, l) >= ed.Call(SCI_GETLINEENDPOSITION, l);
            if (!blank && ed.Call(SCI_GETLINEINDENTATION, l) <= indent) {  // text in the guide's column: a gap
                flush();
                continue;
            }
            int y = static_cast<int>(ed.Call(SCI_POINTYFROMPOSITION, 0, ed.Call(SCI_POSITIONFROMLINE, l))) - shift;
            int lh = static_cast<int>(ed.Call(SCI_TEXTHEIGHT, l));
            if (top < 0) top = y;
            bottom = y + lh;
        }
        flush();
    }
    ReleaseDC(hwnd, dc);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_PAINT) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        Draw(*reinterpret_cast<Editor*>(ref));
        return r;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, Proc, kSubclassId);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
}

namespace BlockGuides {

void Attach(Editor* ed) {
    ed->Call(SCI_MARKERENABLEHIGHLIGHT, 1);  // the fold margin marks the caret's block
    SetWindowSubclass(ed->hwnd(), Proc, kSubclassId, reinterpret_cast<DWORD_PTR>(ed));
}
}
