#include "sticky.h"

#include "editor.h"
#include "scrollbar.h"

namespace {

constexpr UINT_PTR kSubclassId = 0x5CC0;
constexpr int kMaxLines = 4;
constexpr int kHoverMarker = 10;  // line background under the mouse
const UINT kUpdateMessage = RegisterWindowMessageW(L"AsmEditor.StickyLines.Update");

struct State {
    Editor* ed = nullptr;
    HWND host = nullptr;            // child of the editor at its top: the view and a line under it
    std::unique_ptr<Editor> view;   // the sticky lines, styled as in the editor
    std::vector<int> lines;         // shown: 0-based lines of the editor, outermost first
    std::string key;                // what the view shows, to skip updates that change nothing
    std::string styleKey;           // font size and theme the view's styles were made for
    bool pending = false;
    int hover = -1;
    int sectionsVersion = -1;       // Editor::version() the list of sections was made for
    std::vector<int> sections;      // 0-based lines that start a section
};

// Lines that start a section: NASM "section"/"segment" (also in brackets), GAS and MASM section directives.
void ScanSections(State& s) {
    Editor& ed = *s.ed;
    if (s.sectionsVersion == ed.version()) return;
    s.sectionsVersion = ed.version();
    s.sections.clear();
    static const char* const kWords[] = {"section", "segment", ".section", ".text", ".data", ".bss", ".rodata", ".code", ".const"};
    Sci_Position count = ed.Call(SCI_GETLINECOUNT);
    for (Sci_Position line = 0; line < count; line++) {
        Sci_Position start = ed.Call(SCI_GETLINEINDENTPOSITION, line);
        Sci_Position len = std::min<Sci_Position>(ed.Call(SCI_GETLINEENDPOSITION, line) - start, 16);
        if (len < 4) continue;
        const char* p = reinterpret_cast<const char*>(ed.Call(SCI_GETRANGEPOINTER, start, len));
        if (!p) continue;
        std::string word;
        for (Sci_Position i = p[0] == '[' ? 1 : 0; i < len; i++) {
            char c = static_cast<char>(tolower(static_cast<unsigned char>(p[i])));
            if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') break;
            word += c;
        }
        if (std::any_of(std::begin(kWords), std::end(kWords), [&](const char* w) { return word == w; })) {
            s.sections.push_back(static_cast<int>(line));
        }
    }
}

// The lines that open what `line` is in and are above it: its fold parents (global labels, %macro/%if/%rep
// blocks) and the section; outermost first, the innermost kMaxLines.
std::vector<int> Chain(State& s, Sci_Position line) {
    Editor& ed = *s.ed;
    std::vector<int> chain;
    for (Sci_Position p = line; chain.size() < 32;) {
        p = ed.Call(SCI_GETFOLDPARENT, p);
        if (p < 0) break;
        chain.push_back(static_cast<int>(p));
    }
    auto it = std::lower_bound(s.sections.begin(), s.sections.end(), static_cast<int>(line));
    if (it != s.sections.begin() && std::find(chain.begin(), chain.end(), *(it - 1)) == chain.end()) {
        chain.push_back(*(it - 1));
    }
    std::sort(chain.begin(), chain.end());
    if (chain.size() > kMaxLines) chain.erase(chain.begin(), chain.end() - kMaxLines);
    return chain;
}

// The view takes the editor's look but has no caret, folding or margins to click.
void Configure(Editor& v) {
    v.Call(SCI_SETCARETLINEVISIBLEALWAYS, 0);
    v.Call(SCI_SETCARETLINEVISIBLE, 0);
    v.Call(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    v.Call(SCI_SETUNDOCOLLECTION, 0);
    v.Call(SCI_SETMOUSEDWELLTIME, SC_TIME_FOREVER);
    v.Call(SCI_SETMARGINTYPEN, 1, SC_MARGIN_RTEXT);  // the editor's line numbers
    v.Call(SCI_SETMARGINMASKN, 0, 0);
    v.Call(SCI_SETMARGINMASKN, Editor::kFoldMargin, 0);
    for (int m = 0; m < 5; m++) v.Call(SCI_SETMARGINSENSITIVEN, m, 0);
    v.Call(SCI_MARKERDEFINE, kHoverMarker, SC_MARK_BACKGROUND);
    v.Call(SCI_MARKERSETBACK, kHoverMarker, Theme::Hover);
}

void Update(State& s) {
    s.pending = false;
    Editor& ed = *s.ed;
    HWND e = ed.hwnd();
    if (!IsWindowVisible(e)) return;  // brought up to date when shown
    ScanSections(s);
    Sci_Position first = ed.Call(SCI_GETFIRSTVISIBLELINE);
    // The sticky lines cover the top lines of the editor, so they are what the first line below them is in.
    std::vector<int> chain;
    for (int i = 0; i <= kMaxLines; i++) {
        std::vector<int> c = Chain(s, ed.Call(SCI_DOCLINEFROMVISIBLE, first + static_cast<Sci_Position>(chain.size())));
        if (c == chain) break;
        chain = c;
    }
    if (chain.empty()) {
        if (s.host) ShowWindow(s.host, SW_HIDE);
        s.lines.clear();
        s.key.clear();
        return;
    }

    int fontSize = static_cast<int>(ed.Call(SCI_STYLEGETSIZE, STYLE_DEFAULT));
    if (!s.host) {
        s.host = CreateWindowExW(0, L"AsmSticky", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, e, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        s.view = std::make_unique<Editor>(s.host, fontSize);
        ScrollBars::Show(s.view->hwnd(), false, false);
        Configure(*s.view);
        SetWindowSubclass(s.view->hwnd(), [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) -> LRESULT {
            auto* st = reinterpret_cast<State*>(ref);
            Editor& v = *st->view;
            int lh = std::max(1, static_cast<int>(v.Call(SCI_TEXTHEIGHT, 0)));
            auto setHover = [&](int i) {
                if (i == st->hover) return;
                v.Call(SCI_MARKERDELETEALL, kHoverMarker);
                if (i >= 0) v.Call(SCI_MARKERADD, i, kHoverMarker);
                st->hover = i;
            };
            switch (msg) {
            case WM_LBUTTONDOWN: {  // scroll the line to just below the ones that stay above it
                size_t i = static_cast<size_t>(std::max(0, GET_Y_LPARAM(lp) / lh));
                if (i >= st->lines.size()) return 0;
                Editor& ed = *st->ed;
                int line = st->lines[i];
                ed.Call(SCI_ENSUREVISIBLE, line);
                ed.Call(SCI_GOTOLINE, line);
                ed.Call(SCI_SETFIRSTVISIBLELINE, std::max<Sci_Position>(0, ed.Call(SCI_VISIBLEFROMDOCLINE, line) - static_cast<Sci_Position>(i)));
                SetFocus(ed.hwnd());
                return 0;
            }
            case WM_MOUSEMOVE: {
                int i = GET_Y_LPARAM(lp) / lh;
                setHover(i < static_cast<int>(st->lines.size()) ? i : -1);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                setHover(-1);
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return SendMessageW(st->ed->hwnd(), msg, wp, lp);
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_CONTEXTMENU:
                return 0;
            }
            return DefSubclassProc(hwnd, msg, wp, lp);
        }, kSubclassId, reinterpret_cast<DWORD_PTR>(&s));
        ShowWindow(s.view->hwnd(), SW_SHOW);
    }
    Editor& v = *s.view;
    std::string styleKey = std::to_string(fontSize) + "/" + std::to_string(Theme::EditorBg) + "/" + std::to_string(Theme::Text) +
                           "/" + std::to_string(Theme::Label);
    if (styleKey != s.styleKey) {
        s.styleKey = styleKey;
        v.ApplyStyles(fontSize);
        Configure(v);
        s.key.clear();
    }

    RECT rc;
    GetClientRect(e, &rc);
    int width = std::max<int>(0, rc.right - ScrollBars::VerticalWidth(e));
    int lh = static_cast<int>(ed.Call(SCI_TEXTHEIGHT, 0));
    int zoom = static_cast<int>(ed.Call(SCI_GETZOOM)), xOffset = static_cast<int>(ed.Call(SCI_GETXOFFSET));
    std::string text;
    for (int line : chain) {
        if (!text.empty()) text += '\n';
        text += ed.LineText(line);
    }
    std::string key = text + "\x01" + std::to_string(zoom) + "/" + std::to_string(xOffset) + "/" + std::to_string(width) +
                      "/" + std::to_string(lh);
    for (int line : chain) key += "/" + std::to_string(line);
    for (int m = 0; m < 5; m++) key += "/" + std::to_string(ed.Call(SCI_GETMARGINWIDTHN, m));
    if (key == s.key && IsWindowVisible(s.host)) return;
    s.key = key;
    s.lines = chain;

    for (int m = 0; m < 5; m++) v.Call(SCI_SETMARGINWIDTHN, m, ed.Call(SCI_GETMARGINWIDTHN, m));
    v.Call(SCI_SETMARGINLEFT, 0, ed.Call(SCI_GETMARGINLEFT));
    v.Call(SCI_SETZOOM, zoom);
    v.Call(SCI_SETREADONLY, 0);
    v.Call(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(text.c_str()));
    for (size_t i = 0; i < chain.size(); i++) {
        std::string number = std::to_string(chain[i] + 1) + " ";
        v.Call(SCI_MARGINSETTEXT, i, reinterpret_cast<sptr_t>(number.c_str()));
        v.Call(SCI_MARGINSETSTYLE, i, STYLE_LINENUMBER);
    }
    v.Call(SCI_SETREADONLY, 1);
    v.Lex(v.Call(SCI_GETLENGTH));
    s.hover = -1;
    v.Call(SCI_SETFIRSTVISIBLELINE, 0);
    v.Call(SCI_SETXOFFSET, xOffset);

    int height = lh * static_cast<int>(chain.size());
    int border = std::max(1, MulDiv(1, static_cast<int>(GetDpiForWindow(e)), 96));
    SetWindowPos(s.host, HWND_BOTTOM, 0, 0, width, height + border, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(v.hwnd(), nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(s.host, nullptr, FALSE);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {  // the line under the sticky lines
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        SetDCBrushColor(dc, Theme::Border);
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* s = reinterpret_cast<State*>(ref);
    if (msg == kUpdateMessage) {
        Update(*s);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditorProc, kSubclassId);
        delete s;  // its windows went with the editor's
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    // Scintilla repaints after it scrolls, edits or restyles: the sticky lines follow.
    if ((msg == WM_PAINT || msg == WM_SIZE || msg == WM_SHOWWINDOW) && !s->pending) {
        s->pending = true;
        PostMessageW(hwnd, kUpdateMessage, 0, 0);
    }
    return r;
}
}

namespace StickyLines {

void Attach(Editor* ed) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AsmSticky";
        RegisterClassW(&wc);
        registered = true;
    }
    auto* s = new State;
    s->ed = ed;
    SetWindowSubclass(ed->hwnd(), EditorProc, kSubclassId, reinterpret_cast<DWORD_PTR>(s));
}
}
