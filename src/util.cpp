#include "common.h"

#include <algorithm>
#include <fstream>
#include <sstream>

// ---- strings & paths ------------------------------------------------------------------------------------------

std::wstring Widen(const std::string& s, UINT codePage) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(codePage, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(codePage, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& s, UINT codePage) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(codePage, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(n, '\0');
    WideCharToMultiByte(codePage, 0, s.data(), (int)s.size(), a.data(), n, nullptr, nullptr);
    return a;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::wstring::npos) pos = s.size();
        std::wstring part = Trim(s.substr(start, pos - start));
        if (!part.empty()) out.push_back(part);
        start = pos + 1;
    }
    return out;
}

std::wstring Join(const std::vector<std::wstring>& parts, const std::wstring& sep) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

fs::path NormalizePath(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    return (ec ? p : abs).lexically_normal();
}

bool SamePath(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(NormalizePath(a).c_str(), NormalizePath(b).c_str()) == 0;
}

bool IsUnder(const fs::path& p, const fs::path& dir) {
    if (p.empty() || dir.empty()) return false;
    std::wstring a = Lower(NormalizePath(p).wstring());
    std::wstring d = Lower(NormalizePath(dir).wstring());
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    return a.rfind(d, 0) == 0;
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const fs::path& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- theme ----------------------------------------------------------------------------------------------------

void InitDarkMode() {
    // Undocumented but stable uxtheme exports used by many apps to get dark popup menus.
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(Theme::Dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush) flush();
}

void ApplyDarkWindow(HWND hwnd) {
    BOOL dark = Theme::Dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

void ApplyDarkControl(HWND hwnd) {
    SetWindowTheme(hwnd, Theme::Dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

int Scale(int value, int dpi) {
    return MulDiv(value, dpi, 96);
}

HFONT CreateUiFont(int dpi, int points, bool bold) {
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
}

static int CALLBACK FontEnumProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

std::wstring MonospaceFontName() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    HDC dc = GetDC(nullptr);
    for (const wchar_t* name : {L"JetBrains Mono", L"Cascadia Mono", L"Consolas"}) {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, name);
        bool found = false;
        EnumFontFamiliesExW(dc, &lf, FontEnumProc, reinterpret_cast<LPARAM>(&found), 0);
        if (found) {
            cached = name;
            break;
        }
    }
    ReleaseDC(nullptr, dc);
    if (cached.empty()) cached = L"Courier New";
    return cached;
}

// ---- flat button ----------------------------------------------------------------------------------------------

namespace {
struct FlatButtonState {
    DWORD flags = 0;
    bool hover = false;
    bool pressed = false;
    bool checked = false;
    HFONT font = nullptr;
};

LRESULT CALLBACK FlatButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<FlatButtonState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* s = new FlatButtonState();
        s->flags = static_cast<DWORD>(reinterpret_cast<uintptr_t>(cs->lpCreateParams));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        break;
    }
    case WM_NCDESTROY:
        delete st;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case WM_SETFONT:
        st->font = reinterpret_cast<HFONT>(wp);
        if (lp) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(st->font);
    case BM_GETCHECK:
        return st->checked ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
        st->checked = wp == BST_CHECKED;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_MOUSEMOVE:
        if (!st->hover) {
            st->hover = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        st->hover = false;
        st->pressed = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        st->pressed = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        bool was = st->pressed;
        st->pressed = false;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (was && PtInRect(&rc, pt)) SendMessageW(hwnd, BM_CLICK, 0, 0);
        return 0;
    }
    case WM_KEYUP:
        if (wp == VK_SPACE) SendMessageW(hwnd, BM_CLICK, 0, 0);
        return 0;
    case BM_CLICK:
        if (st->flags & FB_CHECK) {
            st->checked = !st->checked;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED),
                     reinterpret_cast<LPARAM>(hwnd));
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETTEXT: {
        LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return r;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        bool enabled = IsWindowEnabled(hwnd);
        COLORREF parentBg = Theme::PanelBg;
        HBRUSH bg = CreateSolidBrush(parentBg);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        wchar_t text[256];
        GetWindowTextW(hwnd, text, 256);
        SelectObject(dc, st->font ? st->font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        int dpi = GetDpiForWindow(hwnd);
        if (st->flags & FB_CHECK) {
            int box = Scale(14, dpi);
            RECT b{0, (rc.bottom - box) / 2, box, (rc.bottom - box) / 2 + box};
            HBRUSH fill = CreateSolidBrush(st->checked ? Theme::Accent : Theme::InputBg);
            HPEN pen = CreatePen(PS_SOLID, 1, st->checked ? Theme::Accent : (st->hover ? Theme::Muted : Theme::Border));
            SelectObject(dc, fill);
            SelectObject(dc, pen);
            RoundRect(dc, b.left, b.top, b.right, b.bottom, 4, 4);
            DeleteObject(fill);
            DeleteObject(pen);
            if (st->checked) {
                HPEN tick = CreatePen(PS_SOLID, std::max(2, Scale(2, dpi)), RGB(255, 255, 255));
                SelectObject(dc, tick);
                MoveToEx(dc, b.left + box / 4, b.top + box / 2, nullptr);
                LineTo(dc, b.left + box * 5 / 12, b.top + box * 3 / 4 - 1);
                LineTo(dc, b.left + box * 3 / 4 + 1, b.top + box / 4);
                DeleteObject(tick);
            }
            RECT t = rc;
            t.left = box + Scale(8, dpi);
            SetTextColor(dc, enabled ? Theme::Text : Theme::Dim);
            DrawTextW(dc, text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        } else {
            COLORREF face = (st->flags & FB_PRIMARY) ? Theme::Accent : Theme::Border;
            if (enabled && st->pressed) face = (st->flags & FB_PRIMARY) ? Theme::Blend(Theme::Accent, 0, 15) : Theme::Pressed;
            else if (enabled && st->hover) face = (st->flags & FB_PRIMARY) ? Theme::Blend(Theme::Accent, 0xFFFFFF, 12) : Theme::Hover;
            HBRUSH fill = CreateSolidBrush(face);
            HPEN pen = CreatePen(PS_SOLID, 1, GetFocus() == hwnd ? Theme::Accent : face);
            SelectObject(dc, fill);
            SelectObject(dc, pen);
            RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, Scale(6, dpi), Scale(6, dpi));
            DeleteObject(fill);
            DeleteObject(pen);
            SetTextColor(dc, !enabled ? Theme::Dim : (st->flags & FB_PRIMARY) ? RGB(255, 255, 255) : Theme::Text);
            DrawTextW(dc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

void RegisterFlatButton(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = FlatButtonProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = L"AsmFlatButton";
    RegisterClassW(&wc);
}

HWND CreateFlatButton(HWND parent, int id, const wchar_t* text, DWORD flags) {
    return CreateWindowExW(0, L"AsmFlatButton", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                           reinterpret_cast<void*>(static_cast<uintptr_t>(flags)));
}

// ---- settings -------------------------------------------------------------------------------------------------

fs::path Settings::Dir() {
    wchar_t* appData = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = fs::path(appData) / L"AsmEditor";
    CoTaskMemFree(appData);
    return dir;
}

static std::map<std::wstring, std::wstring> ReadIni(const fs::path& file) {
    std::map<std::wstring, std::wstring> kv;
    std::ifstream in(file, std::ios::binary);
    if (!in) return kv;
    std::stringstream ss;
    ss << in.rdbuf();
    for (const std::wstring& line : Split(Widen(ss.str()), L'\n')) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || line[0] == L'#') continue;
        kv[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return kv;
}

static void WriteIni(const fs::path& file, const std::vector<std::pair<std::wstring, std::wstring>>& kv) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::wstring text;
    for (const auto& [k, v] : kv) text += k + L"=" + v + L"\r\n";
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    std::string bytes = Narrow(text);
    out.write(bytes.data(), (std::streamsize)bytes.size());
}

void Settings::Load() {
    auto kv = ReadIni(Dir() / L"settings.ini");
    auto get = [&](const wchar_t* k, const std::wstring& def = L"") {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    };
    auto getInt = [&](const wchar_t* k, int def) {
        std::wstring v = get(k);
        return v.empty() ? def : _wtoi(v.c_str());
    };
    nasmPath = get(L"nasm.path");
    nasmArgs = get(L"nasm.args");
    linkerPreset = get(L"linker.preset");
    linkerPath = get(L"linker.path");
    linkerArgs = get(L"linker.args");
    libPaths = Split(get(L"linker.libPaths"), L'|');
    releaseNasmArgs = get(L"release.nasmArgs");
    releaseLinkerArgs = get(L"release.linkerArgs");
    stackCheck = get(L"editor.stackCheck", L"1") == L"1";
    autosave = get(L"editor.autosave") == L"1";
    log = get(L"log") == L"1";
    theme = get(L"ui.theme", L"dark");
    language = get(L"ui.language", L"ru");
    formatIndent = getInt(L"format.indent", 4);
    formatOperands = getInt(L"format.operands", 12);
    formatComments = getInt(L"format.comments", 40);
    keys.clear();
    for (const auto& [k, v] : kv) {
        if (k.rfind(L"key.", 0) == 0) keys[k.substr(4)] = v;
    }
    programEncoding = get(L"run.encoding", L"UTF-8");
    externalConsole = get(L"run.externalConsole") == L"1";
    liveCheck = get(L"editor.liveCheck", L"1") == L"1";
    fontSize = std::clamp(getInt(L"editor.fontSize", 11), 6, 40);
    recent = Split(get(L"recentProjects"), L'|');
    lastProject = get(L"lastProject");
    sessionFiles = Split(get(L"session.files"), L'|');
    window = {getInt(L"window.x", CW_USEDEFAULT), getInt(L"window.y", CW_USEDEFAULT),
              getInt(L"window.w", 0), getInt(L"window.h", 0)};
    maximized = get(L"window.maximized") == L"1";
    treeWidth = getInt(L"layout.treeWidth", 240);
    consoleHeight = getInt(L"layout.consoleHeight", 230);
    debugWidth = getInt(L"layout.debugWidth", 430);
}

void Settings::Save() const {
    std::vector<std::pair<std::wstring, std::wstring>> kv = {
        {L"nasm.path", nasmPath},
        {L"nasm.args", nasmArgs},
        {L"linker.preset", linkerPreset},
        {L"linker.path", linkerPath},
        {L"linker.args", linkerArgs},
        {L"linker.libPaths", Join(libPaths, L"|")},
        {L"run.encoding", programEncoding},
        {L"run.externalConsole", externalConsole ? L"1" : L"0"},
        {L"editor.liveCheck", liveCheck ? L"1" : L"0"},
        {L"editor.fontSize", std::to_wstring(fontSize)},
        {L"recentProjects", Join(recent, L"|")},
        {L"lastProject", lastProject},
        {L"session.files", Join(sessionFiles, L"|")},
        {L"window.x", std::to_wstring(window.left)},
        {L"window.y", std::to_wstring(window.top)},
        {L"window.w", std::to_wstring(window.right)},
        {L"window.h", std::to_wstring(window.bottom)},
        {L"window.maximized", maximized ? L"1" : L"0"},
        {L"layout.treeWidth", std::to_wstring(treeWidth)},
        {L"layout.consoleHeight", std::to_wstring(consoleHeight)},
        {L"layout.debugWidth", std::to_wstring(debugWidth)},
        {L"release.nasmArgs", releaseNasmArgs},
        {L"release.linkerArgs", releaseLinkerArgs},
        {L"editor.stackCheck", stackCheck ? L"1" : L"0"},
        {L"editor.autosave", autosave ? L"1" : L"0"},
        {L"log", log ? L"1" : L"0"},
        {L"ui.theme", theme},
        {L"ui.language", language},
        {L"format.indent", std::to_wstring(formatIndent)},
        {L"format.operands", std::to_wstring(formatOperands)},
        {L"format.comments", std::to_wstring(formatComments)},
    };
    for (const auto& [name, value] : keys) kv.push_back({L"key." + name, value});
    WriteIni(Dir() / L"settings.ini", kv);
}
