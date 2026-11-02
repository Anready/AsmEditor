#include "scrollbar.h"

#include "editor.h"

#include <chrono>
#include <cmath>

namespace {

constexpr UINT_PTR kSubclassId = 0x5CB0;
constexpr UINT_PTR kWheelTimer = 0x5CB1;
constexpr int kBarWidth = 10;  // 96-dpi pixels
constexpr int kMinThumb = 24;
constexpr double kPreciseRest = 120;  // ms without touchpad messages after which the text settles on a whole line
const UINT kSyncMessage = RegisterWindowMessageW(L"AsmEditor.ScrollBars.Sync");
const UINT kMovedMessage = RegisterWindowMessageW(L"AsmEditor.ScrollBars.Moved");

enum class Kind { Scintilla, Tree, ListBox, Edit, Other };

struct State;

// One bar: a child window of the target over its right or bottom edge. Positions are in the target's units (lines,
// pixels, items).
struct Bar {
    State* state = nullptr;
    bool vertical = true;
    HWND hwnd = nullptr;
    RECT rc{};  // in the target's client area; empty while hidden
    int64_t total = 0, page = 0, pos = 0;
    double smooth = 0;  // pos, with the part of a line while Scintilla scrolls smoothly (where the thumb is drawn)
    bool hover = false, drag = false;
    int grab = 0;  // where the thumb was taken, from its start
};

struct State {
    HWND target = nullptr;
    Kind kind = Kind::Other;
    bool wantV = true, wantH = true;
    Bar v, h;
    bool pending = false;  // a sync is posted
    bool syncing = false;  // our own messages to the target do not post another one
    COLORREF bg = 0;
    // The system scrollbars kept out of sight: the window is this much wider / taller than it was given, and clipped.
    int extraW = 0, extraH = 0;
    int visibleW = 0, visibleH = 0;  // the size the owner gave the window
    // Pixel-smooth scrolling of Scintilla (which scrolls by whole lines): its picture is drawn higher by the part of a
    // line (offset). The window itself stays: moving and clipping it again each frame took 30-50 ms (SetWindowRgn).
    int offset = 0;
    HDC memDC = nullptr;  // where Scintilla paints meanwhile, kept for the frames of a glide
    HBITMAP memBmp = nullptr;
    HGDIOBJ memOld = nullptr;
    int memW = 0, memH = 0;
    bool framing = false;       // a frame of smooth scrolling is being put together: nothing reaches the screen yet
    bool smoothing = false;
    double smoothFrom = 0, smoothTo = 0, smoothPos = 0;
    double smoothStart = 0;  // NowMs() when the glide to smoothTo began
    int64_t expectedFirst = -1;  // the first line we set: another one means something else scrolled
    bool hSmoothing = false;     // Scintilla: a sideways glide of the x offset (in pixels)
    double hFrom = 0, hTo = 0, hStart = 0;
    int sciMargin = 1;   // Scintilla: its right margin before the vertical bar covered the edge
    int editMargin = 0;  // multi-line edit: its right margin as created
    double wheelTarget = 0;  // smooth wheel scrolling: the first line it is heading for
    double wheelRest = 0;    // the part of a line left over (touchpads send small steps)
    bool wheeling = false;
    bool precise = false;    // Scintilla follows a touchpad or a high-resolution wheel message by message
    double preciseLast = 0;  // NowMs() of its last message
};

// Milliseconds on a fine clock (GetTickCount steps by 16 ms: too coarse for frames).
double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int Px(HWND hwnd, int v) {
    int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    return MulDiv(v, dpi ? dpi : 96, 96);
}

State* Find(HWND hwnd);
LRESULT CALLBACK TargetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref);

LRESULT Send(State& s, UINT msg, WPARAM wp = 0, LPARAM lp = 0) {
    bool was = s.syncing;
    s.syncing = true;
    LRESULT r = SendMessageW(s.target, msg, wp, lp);
    s.syncing = was;
    return r;
}

// The color behind the bars: the control's own background.
COLORREF Background(State& s) {
    if (s.kind == Kind::Scintilla) return static_cast<COLORREF>(Send(s, SCI_STYLEGETBACK, STYLE_DEFAULT));
    if (s.kind == Kind::Tree) {
        COLORREF c = static_cast<COLORREF>(Send(s, TVM_GETBKCOLOR));
        return c == static_cast<COLORREF>(-1) ? GetSysColor(COLOR_WINDOW) : c;
    }
    HWND parent = GetParent(s.target);
    wchar_t cls[32] = L"";
    if (parent) GetClassNameW(parent, cls, 32);
    if (wcscmp(cls, L"ListBoxX") == 0) return Theme::PanelBg;  // Scintilla's completion list
    HDC dc = GetDC(s.target);
    auto brush = reinterpret_cast<HBRUSH>(SendMessageW(parent, s.kind == Kind::Edit ? WM_CTLCOLOREDIT : WM_CTLCOLORLISTBOX,
                                                       reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(s.target)));
    ReleaseDC(s.target, dc);
    LOGBRUSH lb{};
    if (brush && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_SOLID) return lb.lbColor;
    return Theme::PanelBg;
}

void Measure(State& s) {
    HWND t = s.target;
    DWORD style = static_cast<DWORD>(GetWindowLongW(t, GWL_STYLE));
    auto native = [&](int which, Bar& b, DWORD flag) {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        bool shown = (style & flag) && GetScrollInfo(t, which, &si);
        b.total = shown ? static_cast<int64_t>(si.nMax) - si.nMin + 1 : 0;
        b.page = shown ? std::max<int64_t>(1, si.nPage) : 0;
        b.pos = shown ? si.nPos - si.nMin : 0;
    };
    if (s.kind == Kind::Scintilla) {
        // The scroll range of the editors: the lines plus a few past the end (Editor::LimitScroll).
        int64_t last = Send(s, SCI_GETLINECOUNT) - 1;
        s.v.total = Send(s, SCI_VISIBLEFROMDOCLINE, last) + Send(s, SCI_WRAPCOUNT, last) + Editor::kScrollPastEnd;
        s.v.page = Send(s, SCI_LINESONSCREEN);
        s.v.pos = Send(s, SCI_GETFIRSTVISIBLELINE);
        s.v.smooth = s.smoothing ? s.smoothPos : static_cast<double>(s.v.pos);
    } else {
        native(SB_VERT, s.v, WS_VSCROLL);
        s.v.smooth = static_cast<double>(s.v.pos);
    }
    native(SB_HORZ, s.h, WS_HSCROLL);  // Scintilla keeps its (hidden) horizontal scrollbar: it tracks the line width
    s.bg = Background(s);
}

LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void Place(Bar& b, RECT r, bool show) {
    if (!show) r = {};
    if (EqualRect(&r, &b.rc) && (!show || (b.hwnd && IsWindowVisible(b.hwnd)))) return;
    b.rc = r;
    if (!show) {
        if (b.hwnd) ShowWindow(b.hwnd, SW_HIDE);
        return;
    }
    if (!b.hwnd) {
        b.hwnd = CreateWindowExW(0, L"AsmScrollBar", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, b.state->target, nullptr,
                                 GetModuleHandleW(nullptr), &b);
    }
    // Within a frame of smooth scrolling the bar only moves (copying its old pixels on the screen would show a torn
    // thumb before the frame is painted); the frame paints it with everything else.
    UINT quiet = b.state->framing ? SWP_NOREDRAW | SWP_NOCOPYBITS : 0;
    SetWindowPos(b.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | quiet);
    InvalidateRect(b.hwnd, nullptr, FALSE);  // all of it: the thumb moves with the size
}

void Sync(State& s) {
    s.pending = false;
    if (!IsWindow(s.target)) return;
    int64_t before[] = {s.v.total, s.v.page, s.v.pos, s.h.total, s.h.page, s.h.pos};
    double smoothBefore = s.v.smooth;
    COLORREF bg = s.bg;
    bool was = s.syncing;
    s.syncing = true;
    Measure(s);
    int w = Px(s.target, kBarWidth);
    // Nothing is written under the vertical bar: Scintilla gets a right margin as wide, an edit as well (plus the
    // width of its hidden scrollbar while that is not there, so its lines wrap the same either way).
    if (s.kind == Kind::Scintilla) {
        int margin = s.wantV ? w : s.sciMargin;
        if (Send(s, SCI_GETMARGINRIGHT) != margin) Send(s, SCI_SETMARGINRIGHT, 0, margin);
    } else if (s.kind == Kind::Edit) {
        bool bar = (GetWindowLongW(s.target, GWL_STYLE) & WS_VSCROLL) != 0;
        int margin = s.editMargin + w + (bar ? 0 : s.extraW);
        if (HIWORD(Send(s, EM_GETMARGINS)) != margin) Send(s, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, margin));
    }
    s.syncing = was;

    RECT rc;
    GetClientRect(s.target, &rc);
    // Scintilla scrolls a few lines past the end: no thumb while the lines themselves fit (unless scrolled down).
    int64_t pastEnd = s.kind == Kind::Scintilla ? Editor::kScrollPastEnd : 0;
    bool needV = s.wantV && s.v.page > 0 && s.v.page < s.v.total && (s.v.pos > 0 || s.v.page < s.v.total - pastEnd);
    bool needH = s.wantH && s.h.page > 0 && s.h.page < s.h.total;
    Place(s.v, {rc.right - w, rc.top, rc.right, rc.bottom - (needH ? w : 0)}, needV);
    Place(s.h, {0, rc.bottom - w, rc.right - (needV ? w : 0), rc.bottom}, needH);
    int64_t after[] = {s.v.total, s.v.page, s.v.pos, s.h.total, s.h.page, s.h.pos};
    if (bg != s.bg || smoothBefore != s.v.smooth || !std::equal(std::begin(before), std::end(before), std::begin(after))) {
        if (s.v.hwnd) InvalidateRect(s.v.hwnd, nullptr, FALSE);
        if (s.h.hwnd) InvalidateRect(s.h.hwnd, nullptr, FALSE);
    }
}

void PostSync(State& s) {
    if (s.pending || s.syncing) return;
    s.pending = true;
    PostMessageW(s.target, kSyncMessage, 0, 0);
}

// Scrolls the target so that pos (in the bar's units) is the first visible line / item / pixel column.
void ScrollTo(State& s, bool vertical, int64_t pos) {
    Measure(s);
    const Bar& b = vertical ? s.v : s.h;
    pos = std::clamp<int64_t>(pos, 0, std::max<int64_t>(0, b.total - b.page));
    int64_t delta = pos - b.pos;
    if (!delta) return;
    if (s.kind == Kind::Scintilla) {
        SendMessageW(s.target, vertical ? SCI_SETFIRSTVISIBLELINE : SCI_SETXOFFSET, static_cast<WPARAM>(pos), 0);
    } else if (s.kind == Kind::ListBox && vertical) {
        SendMessageW(s.target, LB_SETTOPINDEX, static_cast<WPARAM>(pos), 0);
    } else if (s.kind == Kind::Edit && vertical) {
        SendMessageW(s.target, EM_LINESCROLL, 0, static_cast<LPARAM>(delta));
    } else {  // tree views and the rest: a line at a time
        UINT msg = vertical ? WM_VSCROLL : WM_HSCROLL;
        bool many = std::abs(delta) > 1;
        if (many) SendMessageW(s.target, WM_SETREDRAW, FALSE, 0);
        for (int64_t i = 0; i < std::abs(delta); i++) SendMessageW(s.target, msg, delta > 0 ? SB_LINEDOWN : SB_LINEUP, 0);
        SendMessageW(s.target, msg, SB_ENDSCROLL, 0);
        if (many) {
            SendMessageW(s.target, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(s.target, nullptr, TRUE);
        }
    }
}

// ---- smooth wheel scrolling -------------------------------------------------------------------------------------

void FreeMemory(State& s) {
    if (!s.memDC) return;
    SelectObject(s.memDC, s.memOld);
    DeleteObject(s.memBmp);
    DeleteDC(s.memDC);
    s.memDC = nullptr;
    s.memBmp = nullptr;
}

// Scintilla while its picture is drawn higher by `offset` pixels: it paints into memory, which is copied up. The
// strip left at the bottom (the next line, which Scintilla does not paint) gets the editor's background.
void PaintShifted(State& s) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(s.target, &ps);
    RECT rc;
    GetClientRect(s.target, &rc);
    int w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
    if (!s.memDC || s.memW != w || s.memH != h) {
        FreeMemory(s);
        s.memDC = CreateCompatibleDC(dc);
        s.memBmp = CreateCompatibleBitmap(dc, w, h);
        s.memOld = SelectObject(s.memDC, s.memBmp);
        s.memW = w;
        s.memH = h;
    }
    DefSubclassProc(s.target, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(s.memDC), PRF_CLIENT);
    BitBlt(dc, 0, 0, w, h - s.offset, s.memDC, 0, s.offset, SRCCOPY);
    RECT strip{0, h - s.offset, w, h};
    SetDCBrushColor(dc, s.bg);
    FillRect(dc, &strip, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    EndPaint(s.target, &ps);
}

// One frame of smooth scrolling, shown at once: the sticky lines and bars in place, everything painted together,
// and whoever draws a scrollbar for the window outside it (the editor's mark bar) moves its thumb along.
void Frame(State& s) {
    s.framing = true;
    Sync(s);
    s.framing = false;
    // The window does not move: its children (bars, sticky lines) are painted only where they changed. At once, as
    // wheel messages come before WM_PAINT and would hold them back while the wheel spins.
    RedrawWindow(s.target, nullptr, nullptr, RDW_INVALIDATE | RDW_NOCHILDREN | RDW_UPDATENOW);
    RedrawWindow(s.target, nullptr, nullptr, RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (HWND parent = GetParent(s.target)) SendMessageW(parent, kMovedMessage, reinterpret_cast<WPARAM>(s.target), 0);
}

void StopWheel(State& s) {
    s.wheeling = false;
    s.precise = false;
    if (s.smoothing) {
        s.smoothing = false;
        s.offset = 0;
        Frame(s);
        FreeMemory(s);
    }
    if (!s.hSmoothing) KillTimer(s.target, kWheelTimer);
}

// Scintilla sideways: the same eased glide of its x offset, which is in pixels already.
void SideStep(State& s) {
    double t = std::min(1.0, (NowMs() - s.hStart) / 110.0);
    double eased = 1 - (1 - t) * (1 - t) * (1 - t);
    Send(s, SCI_SETXOFFSET, static_cast<WPARAM>(std::lround(s.hFrom + (s.hTo - s.hFrom) * eased)));
    UpdateWindow(s.target);
    Sync(s);
    if (t >= 1) {
        s.hSmoothing = false;
        if (!s.wheeling) KillTimer(s.target, kWheelTimer);
    }
}

bool SideWheel(State& s, int delta) {
    if (s.kind != Kind::Scintilla) return false;
    Measure(s);
    int64_t maxX = s.h.total - s.h.page;
    if (!s.wantH || s.h.page <= 0 || maxX <= 0) return false;
    UINT chars = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    int charW = std::max(1, static_cast<int>(Send(s, SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<LPARAM>("x"))));
    double base = s.hSmoothing ? s.hTo : static_cast<double>(Send(s, SCI_GETXOFFSET));
    double to = std::clamp(base + delta / static_cast<double>(WHEEL_DELTA) * chars * charW, 0.0, static_cast<double>(maxX));
    if (std::lround(to) == std::lround(base)) return true;  // at an end: nothing to glide to
    s.hFrom = static_cast<double>(Send(s, SCI_GETXOFFSET));
    s.hTo = to;
    s.hStart = NowMs() - 16;
    if (!s.hSmoothing) {
        s.hSmoothing = true;
        SetTimer(s.target, kWheelTimer, 8, nullptr);
    }
    SideStep(s);
    return true;
}

// Scintilla shows its text from `pos` on (in lines, with the part of a line): the whole lines it scrolls itself,
// the part is drawn higher. `paint`: the frame at once (else with the next one).
void ShowAt(State& s, double pos, int64_t first, bool paint) {
    int lineHeight = std::max(1, static_cast<int>(Send(s, SCI_TEXTHEIGHT, 0)));
    int64_t top = static_cast<int64_t>(std::floor(pos));
    int offset = static_cast<int>(std::lround((pos - static_cast<double>(top)) * lineHeight));
    if (offset >= lineHeight) {
        top++;
        offset = 0;
    }
    if (top != first) Send(s, SCI_SETFIRSTVISIBLELINE, static_cast<WPARAM>(top));
    s.offset = offset;
    s.expectedFirst = Send(s, SCI_GETFIRSTVISIBLELINE);
    if (paint) Frame(s);
}

// Scintilla: an eased glide from where it is to the target, a frame at a time, by pixels.
void SmoothStep(State& s) {
    int64_t first = Send(s, SCI_GETFIRSTVISIBLELINE);
    if (s.expectedFirst >= 0 && first != s.expectedFirst) {  // scrolled by something else (keys, the caret)
        StopWheel(s);
        return;
    }
    if (s.precise) {  // the touchpad moves the text itself; when it stops sending, a short glide to a whole line
        if (NowMs() - s.preciseLast < kPreciseRest) return;
        s.precise = false;
        s.smoothFrom = s.smoothPos;
        s.smoothTo = std::round(s.smoothPos);
        s.smoothStart = NowMs() - 16;
    }
    double t = std::min(1.0, (NowMs() - s.smoothStart) / 110.0);
    double eased = 1 - (1 - t) * (1 - t) * (1 - t);
    s.smoothPos = t >= 1 ? std::round(s.smoothTo) : s.smoothFrom + (s.smoothTo - s.smoothFrom) * eased;
    ShowAt(s, s.smoothPos, first, true);
    if (t >= 1) {
        s.smoothing = false;
        StopWheel(s);
    }
}

// One step towards the target: a part of the way left, at least a line, so a notch takes a few frames.
void WheelStep(State& s) {
    if (s.smoothing) {
        SmoothStep(s);
        return;
    }
    Measure(s);
    double diff = s.wheelTarget - static_cast<double>(s.v.pos);
    if (std::abs(diff) < 1) {
        s.wheelRest = diff;
        StopWheel(s);
        return;
    }
    int64_t step = std::max<int64_t>(1, static_cast<int64_t>(std::ceil(std::abs(diff) * 0.3)));
    int64_t from = s.v.pos;
    ScrollTo(s, true, from + (diff > 0 ? step : -step));
    Measure(s);
    if (s.v.pos == from) {  // at an end
        s.wheelRest = 0;
        StopWheel(s);
        return;
    }
    UpdateWindow(s.target);
    Sync(s);
}

bool Wheel(State& s, WPARAM wp) {
    if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) return false;  // zoom
    if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) return SideWheel(s, -GET_WHEEL_DELTA_WPARAM(wp));  // sideways
    if (s.kind == Kind::Scintilla && Send(s, SCI_AUTOCACTIVE)) return false;
    Measure(s);
    int64_t maxPos = s.v.total - s.v.page;
    if (s.v.page <= 0 || maxPos <= 0) return false;
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    double perNotch = lines == WHEEL_PAGESCROLL ? static_cast<double>(std::max<int64_t>(1, s.v.page - 1)) : lines;
    double delta = -GET_WHEEL_DELTA_WPARAM(wp) / static_cast<double>(WHEEL_DELTA) * perNotch;
    int raw = GET_WHEEL_DELTA_WPARAM(wp);
    if (s.kind == Kind::Scintilla && (raw % WHEEL_DELTA != 0 || (s.precise && NowMs() - s.preciseLast < kPreciseRest))) {
        // A touchpad or a high-resolution wheel: parts of a notch, often many at once, with its own inertia. Each
        // message moves the text by as much, at once (a glide for each would start over every few ms and lag
        // behind); when they stop, the text settles on a whole line (SmoothStep).
        double from = s.smoothing ? s.smoothPos : static_cast<double>(s.v.pos);
        double pos = std::clamp(from + delta, 0.0, static_cast<double>(maxPos));
        s.precise = true;
        s.preciseLast = NowMs();
        s.wheelRest = 0;
        if (pos == from) return true;
        s.smoothFrom = s.smoothTo = s.smoothPos = pos;
        if (!s.smoothing) {
            s.smoothing = s.wheeling = true;
            SetTimer(s.target, kWheelTimer, 8, nullptr);
        }
        // More wheel messages waiting (they come faster than a frame is painted): the last of them paints.
        MSG next;
        bool more = PeekMessageW(&next, nullptr, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_NOREMOVE | PM_NOYIELD) != 0;
        ShowAt(s, pos, s.v.pos, !more);
        return true;
    }
    if (s.kind == Kind::Scintilla) {  // by pixels: from where it is now to the new target
        // The target is a whole line, so a glide ends without a snap; touchpads and free-spinning wheels send
        // parts of a notch: they add up until they make a line. At the ends, nothing starts (no shaking there).
        double base = (s.smoothing ? s.smoothTo : static_cast<double>(s.v.pos)) + s.wheelRest;
        double target = base + delta;
        double clamped = std::clamp(target, 0.0, static_cast<double>(maxPos));
        double to = std::round(clamped);
        s.wheelRest = clamped == target ? clamped - to : 0;
        if (to == (s.smoothing ? s.smoothTo : static_cast<double>(s.v.pos))) return true;
        s.smoothFrom = s.smoothing ? s.smoothPos : static_cast<double>(s.v.pos);
        s.smoothTo = to;
        // As if a frame had passed: the first step moves at once, under the wheel, not a frame later.
        s.smoothStart = NowMs() - 16;
        s.expectedFirst = s.v.pos;
        if (!s.smoothing) {
            s.smoothing = s.wheeling = true;
            SetTimer(s.target, kWheelTimer, 8, nullptr);
        }
        SmoothStep(s);
        return true;
    }
    if (!s.wheeling) s.wheelTarget = static_cast<double>(s.v.pos) + s.wheelRest;
    s.wheelTarget = std::clamp(s.wheelTarget + delta, 0.0, static_cast<double>(maxPos));
    if (!s.wheeling) {
        s.wheeling = true;
        SetTimer(s.target, kWheelTimer, 10, nullptr);
        WheelStep(s);
    }
    return true;
}

// ---- the bar window ---------------------------------------------------------------------------------------------

RECT Thumb(const Bar& b) {
    RECT rc;
    GetClientRect(b.hwnd, &rc);
    int len = b.vertical ? rc.bottom : rc.right;
    if (b.page <= 0 || b.page >= b.total || len <= 0) return {};
    int64_t range = b.total - b.page;
    int thumb = std::max(std::min(Px(b.hwnd, kMinThumb), len), static_cast<int>(b.page * len / b.total));
    double pos = b.vertical ? b.smooth : static_cast<double>(b.pos);
    int at = static_cast<int>(std::lround(std::clamp(pos, 0.0, static_cast<double>(range)) * (len - thumb) / range));
    int inset = Px(b.hwnd, 2);
    return b.vertical ? RECT{inset, at, rc.right - inset, at + thumb} : RECT{at, inset, at + thumb, rc.bottom - inset};
}

void Paint(Bar& b, HDC dc) {
    RECT rc;
    GetClientRect(b.hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    COLORREF bg = b.state->bg;
    SetDCBrushColor(mem, bg);
    FillRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RECT t = Thumb(b);
    if (t.right > t.left && t.bottom > t.top) {
        // As the editor's mark bar: a rounded thumb, brighter while under the mouse or dragged.
        bool active = b.hover || b.drag;
        SetDCBrushColor(mem, Theme::Blend(bg, Theme::Muted, active ? 55 : 30));
        HGDIOBJ oldPen = SelectObject(mem, GetStockObject(NULL_PEN));
        HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(DC_BRUSH));
        int d = b.vertical ? t.right - t.left : t.bottom - t.top;
        RoundRect(mem, t.left, t.top, t.right + 1, t.bottom + 1, d, d);
        SelectObject(mem, oldPen);
        SelectObject(mem, oldBrush);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Dragging the thumb scrolls; a click beside it moves the thumb there (centered) and drags it.
void Mouse(Bar& b, UINT msg, LPARAM lp) {
    State& s = *b.state;
    if (msg == WM_MOUSELEAVE || msg == WM_CAPTURECHANGED) {
        (msg == WM_MOUSELEAVE ? b.hover : b.drag) = false;
        InvalidateRect(b.hwnd, nullptr, FALSE);
        return;
    }
    int at = b.vertical ? GET_Y_LPARAM(lp) : GET_X_LPARAM(lp);
    RECT rc;
    GetClientRect(b.hwnd, &rc);
    RECT thumb = Thumb(b);
    int t0 = b.vertical ? thumb.top : thumb.left, t1 = b.vertical ? thumb.bottom : thumb.right;
    int len = b.vertical ? rc.bottom : rc.right;
    auto scrollTo = [&](int start) {
        int track = std::max(1, len - (t1 - t0));
        StopWheel(s);
        ScrollTo(s, b.vertical, static_cast<int64_t>(std::clamp(start, 0, track)) * (b.total - b.page) / track);
        UpdateWindow(s.target);
        Sync(s);
    };
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (t1 <= t0) return;
        bool on = at >= t0 && at < t1;
        b.grab = on ? at - t0 : (t1 - t0) / 2;
        if (!on) scrollTo(at - b.grab);
        b.drag = true;
        SetCapture(b.hwnd);
        InvalidateRect(b.hwnd, nullptr, FALSE);
        return;
    }
    case WM_MOUSEMOVE:
        if (b.drag) {
            scrollTo(at - b.grab);
        } else if (!b.hover) {
            b.hover = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, b.hwnd, 0};
            TrackMouseEvent(&tme);
        }
        InvalidateRect(b.hwnd, nullptr, FALSE);
        return;
    case WM_LBUTTONUP:
        if (b.drag) ReleaseCapture();
        return;
    }
}

LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* b = reinterpret_cast<Bar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        b = static_cast<Bar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        b->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (b) Paint(*b, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_MOUSELEAVE:
    case WM_CAPTURECHANGED:
        if (b) Mouse(*b, msg == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : msg, lp);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (b) return SendMessageW(b->state->target, msg, wp, lp);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- the target -------------------------------------------------------------------------------------------------

// Messages after which the scroll position or range may have changed.
bool Affects(UINT msg) {
    switch (msg) {
    case WM_PAINT:
    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_KEYDOWN:
    case WM_LBUTTONUP:
    case WM_TIMER:
    case WM_SETTEXT:
    case WM_SETREDRAW:
    case WM_STYLECHANGED:
    case WM_SHOWWINDOW:
        return true;
    }
    return (msg >= EM_GETSEL && msg <= 0x00FF) || (msg >= LB_ADDSTRING && msg <= 0x01FF) || (msg >= WM_USER && msg < 0xC000);
}

// Re-applies the extra size of the hidden system scrollbars (after it changed).
void Resize(State& s) {
    SetWindowPos(s.target, nullptr, 0, 0, s.visibleW, s.visibleH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK TargetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* s = reinterpret_cast<State*>(ref);
    if (msg == kSyncMessage) {
        Sync(*s);
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == kWheelTimer) {
            if (s->hSmoothing) SideStep(*s);
            if (s->wheeling) WheelStep(*s);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (Wheel(*s, wp)) return 0;
        break;
    case WM_MOUSEHWHEEL:  // tilt wheels and touchpads: positive is to the right
        if (SideWheel(*s, GET_WHEEL_DELTA_WPARAM(wp))) return 0;
        break;
    case WM_WINDOWPOSCHANGING: {
        auto* p = reinterpret_cast<WINDOWPOS*>(lp);
        if (!(p->flags & SWP_NOSIZE)) {
            s->visibleW = p->cx;
            s->visibleH = p->cy;
            p->cx += s->extraW;
            p->cy += s->extraH;
        }
        break;
    }
    case WM_WINDOWPOSCHANGED: {
        auto* p = reinterpret_cast<WINDOWPOS*>(lp);
        if (!(p->flags & SWP_NOSIZE)) {
            bool clip = s->extraW || s->extraH;
            SetWindowRgn(hwnd, clip ? CreateRectRgn(0, 0, s->visibleW, s->visibleH) : nullptr, TRUE);
        }
        break;
    }
    case WM_PAINT:
        if (s->offset) {
            PaintShifted(*s);
            PostSync(*s);
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (s->smoothing) StopWheel(*s);
        break;
    case WM_SIZE: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        Sync(*s);  // at once, so the bars do not lag behind the edge
        return r;
    }
    case WM_NCDESTROY:
        KillTimer(hwnd, kWheelTimer);
        RemoveWindowSubclass(hwnd, TargetProc, kSubclassId);
        FreeMemory(*s);
        delete s;
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (Affects(msg)) PostSync(*s);
    return r;
}

State* Find(HWND hwnd) {
    DWORD_PTR ref = 0;
    if (!hwnd || !GetWindowSubclass(hwnd, TargetProc, kSubclassId, &ref)) return nullptr;
    return reinterpret_cast<State*>(ref);
}

std::vector<HWND> g_later;

void CALLBACK AttachPending(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    std::vector<HWND> pending;
    pending.swap(g_later);
    for (HWND h : pending) ScrollBars::Attach(h);
}
}

namespace ScrollBars {

void Attach(HWND hwnd) {
    if (!IsWindow(hwnd) || Find(hwnd)) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = BarProc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"AsmScrollBar";
        RegisterClassW(&wc);
        registered = true;
    }
    wchar_t cls[64] = L"";
    GetClassNameW(hwnd, cls, 64);
    Kind kind = _wcsicmp(cls, L"Scintilla") == 0       ? Kind::Scintilla
                : _wcsicmp(cls, WC_TREEVIEWW) == 0     ? Kind::Tree
                : _wcsicmp(cls, L"ListBox") == 0       ? Kind::ListBox
                : _wcsicmp(cls, L"Edit") == 0          ? Kind::Edit
                                                       : Kind::Other;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    // Tree views add their scrollbar when they need it (list boxes drop it while empty); other controls have one
    // only when created with it.
    if ((kind == Kind::Edit || kind == Kind::Other) && !(style & WS_VSCROLL)) return;

    auto* s = new State;
    s->target = hwnd;
    s->kind = kind;
    s->v.state = s->h.state = s;
    s->h.vertical = false;
    int dpi = static_cast<int>(GetDpiForWindow(hwnd));
    if (!dpi) dpi = 96;
    if (kind == Kind::Scintilla) {
        // The vertical bar is ours; the horizontal one stays (Scintilla tracks the width of the lines only while it
        // shows one), below the bottom edge.
        s->sciMargin = static_cast<int>(SendMessageW(hwnd, SCI_GETMARGINRIGHT, 0, 0));
        SendMessageW(hwnd, SCI_SETVSCROLLBAR, 0, 0);
        SendMessageW(hwnd, SCI_SETHSCROLLBAR, 1, 0);
        s->extraH = GetSystemMetricsForDpi(SM_CYHSCROLL, dpi);
    } else {
        s->wantH = false;
        s->extraW = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
        if (kind == Kind::Edit) s->editMargin = HIWORD(SendMessageW(hwnd, EM_GETMARGINS, 0, 0));
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN);
    }
    RECT wr;
    GetWindowRect(hwnd, &wr);
    s->visibleW = wr.right - wr.left;
    s->visibleH = wr.bottom - wr.top;
    SetWindowSubclass(hwnd, TargetProc, kSubclassId, reinterpret_cast<DWORD_PTR>(s));
    Resize(*s);
    PostSync(*s);
}

void AttachLater(HWND hwnd) {
    if (g_later.empty()) SetTimer(nullptr, 0, 0, AttachPending);
    g_later.push_back(hwnd);
}

void Show(HWND hwnd, bool vertical, bool horizontal) {
    State* s = Find(hwnd);
    if (!s || (s->wantV == vertical && s->wantH == horizontal)) return;
    s->wantV = vertical;
    if (s->kind == Kind::Scintilla && s->wantH != horizontal) {
        s->wantH = horizontal;
        Send(*s, SCI_SETHSCROLLBAR, horizontal ? 1 : 0);
        int dpi = static_cast<int>(GetDpiForWindow(hwnd));
        s->extraH = horizontal ? GetSystemMetricsForDpi(SM_CYHSCROLL, dpi ? dpi : 96) : 0;
        Resize(*s);
    }
    Sync(*s);
}

UINT MovedMessage() { return kMovedMessage; }

double Position(HWND hwnd) {
    State* s = Find(hwnd);
    if (s && s->smoothing) return s->smoothPos;
    return static_cast<double>(SendMessageW(hwnd, SCI_GETFIRSTVISIBLELINE, 0, 0));
}

int TopOffset(HWND hwnd) {
    State* s = Find(hwnd);
    return s ? s->offset : 0;
}

int VerticalWidth(HWND hwnd) {
    State* s = Find(hwnd);
    return s && s->v.rc.right > s->v.rc.left ? s->v.rc.right - s->v.rc.left : 0;
}

void Changed(HWND hwnd) {
    if (State* s = Find(hwnd)) PostSync(*s);
}
}
