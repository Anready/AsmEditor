#include "common.h"

#include <set>

namespace {

constexpr int kBrowseNasm = 101;
constexpr int kBrowseLinker = 102;
constexpr int kPreset = 103;
constexpr int kEncoding = 104;
constexpr int kDetect = 105;
constexpr int kExternal = 106;
constexpr int kLive = 107;
constexpr int kTheme = 108;
constexpr int kLanguage = 109;
constexpr int kStackCheck = 110;
constexpr int kAutosave = 111;
constexpr int kLog = 112;
constexpr int kMatchCase = 113;
constexpr int kWholeWord = 114;
constexpr int kRegex = 115;
constexpr int kReplaceAll = 116;
constexpr int kOutput = 117;
constexpr int kPlatform = 118;
constexpr int kResetKeys = 119;
constexpr int kFormatOnSave = 120;

// Modal dark dialog built from plain child windows (no resource files).
class Dialog {
public:
    std::function<bool()> onOk;
    std::function<void(int)> onCommand;
    std::function<void(HWND control, int code)> onControl;  // EN_CHANGE, LBN_DBLCLK… of child controls

    Dialog(HWND owner, const std::wstring& title, int width, int height) : owner_(owner) {
        dpi_ = owner ? GetDpiForWindow(owner) : 96;
        font_ = CreateUiFont(dpi_);
        bg_ = CreateSolidBrush(Theme::PanelBg);
        inputBg_ = CreateSolidBrush(Theme::InputBg);
        RECT rc{0, 0, S(width), S(height)};
        DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME, dpi_);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        RECT orc{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (owner) GetWindowRect(owner, &orc);
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AsmDialog", title.c_str(), style,
                                orc.left + (orc.right - orc.left - w) / 2, orc.top + (orc.bottom - orc.top - h) / 2,
                                w, h, owner, nullptr, GetModuleHandleW(nullptr), this);
        ApplyDarkWindow(hwnd_);
    }

    ~Dialog() {
        DeleteObject(font_);
        DeleteObject(bg_);
        DeleteObject(inputBg_);
    }

    int S(int v) const { return Scale(v, dpi_); }
    HWND hwnd() const { return hwnd_; }
    void Finish(bool ok) {
        if (ok && onOk && !onOk()) return;
        ok_ = ok;
        done_ = true;
    }

    HWND Label(const std::wstring& text, int x, int y, int w, int h, bool muted = false, bool bold = false) {
        HWND l = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                 S(x), S(y), S(w), S(h), hwnd_, nullptr, nullptr, nullptr);
        if (muted) muted_.insert(l);
        if (bold) {
            if (!boldFont_) boldFont_ = CreateUiFont(dpi_, 9, true);
            SendMessageW(l, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), FALSE);
        } else {
            SendMessageW(l, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        }
        return l;
    }

    HWND Edit(const std::wstring& text, int x, int y, int w, int h, bool multiline = false) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        if (multiline) style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
        RECT frame{S(x), S(y), S(x + w), S(y + h)};
        int top = frame.top + 1, height = frame.bottom - frame.top - 2;
        if (!multiline) {  // single-line edits do not center text themselves
            HDC dc = GetDC(hwnd_);
            HGDIOBJ old = SelectObject(dc, font_);
            TEXTMETRICW tm;
            GetTextMetricsW(dc, &tm);
            SelectObject(dc, old);
            ReleaseDC(hwnd_, dc);
            height = std::min(height, static_cast<int>(tm.tmHeight) + 2);
            top = frame.top + (frame.bottom - frame.top - height) / 2;
        }
        HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), style, frame.left + 1, top, frame.right - frame.left - 2,
                                 height, hwnd_, nullptr, nullptr, nullptr);
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
        if (multiline) ApplyDarkControl(e);
        edits_.push_back({e, frame});
        if (!firstFocus_) firstFocus_ = e;
        return e;
    }

    HWND List(int x, int y, int w, int h) {
        HWND l = CreateWindowExW(0, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_USETABSTOPS |
                                     LBS_NOINTEGRALHEIGHT,
                                 S(x), S(y), S(w), S(h), hwnd_, nullptr, nullptr, nullptr);
        SendMessageW(l, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        int tab = w * 4 * 55 / 100 / std::max(1, static_cast<int>(LOWORD(GetDialogBaseUnits())));  // detail column
        SendMessageW(l, LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tab));
        ApplyDarkControl(l);
        return l;
    }

    HWND Button(int id, const std::wstring& text, int x, int y, int w, int h, DWORD flags = 0) {
        HWND b = CreateFlatButton(hwnd_, id, text.c_str(), flags);
        SetWindowPos(b, nullptr, S(x), S(y), S(w), S(h), SWP_NOZORDER);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return b;
    }

    bool Run() {
        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        if (firstFocus_) {
            SetFocus(firstFocus_);
            SendMessageW(firstFocus_, EM_SETSEL, 0, -1);
        }
        MSG msg;
        while (!done_) {
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        EnableWindow(owner_, TRUE);
        if (owner_) SetForegroundWindow(owner_);
        DestroyWindow(hwnd_);
        return ok_;
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Dialog* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, self->bg_);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            HBRUSH border = CreateSolidBrush(Theme::Border);
            for (auto [e, frame] : self->edits_) {
                RECT r = frame;
                FillRect(dc, &r, self->inputBg_);
                SetDCBrushColor(dc, Theme::Accent);
                FrameRect(dc, &r, GetFocus() == e ? (HBRUSH)GetStockObject(DC_BRUSH) : border);
            }
            DeleteObject(border);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, self->muted_.count(reinterpret_cast<HWND>(lp)) ? Theme::Muted : Theme::Text);
            SetBkColor(dc, Theme::PanelBg);
            return reinterpret_cast<LRESULT>(self->bg_);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, Theme::Bright);
            SetBkColor(dc, Theme::InputBg);
            return reinterpret_cast<LRESULT>(self->inputBg_);
        }
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (lp && self->onControl && reinterpret_cast<HWND>(lp) != hwnd) {
                self->onControl(reinterpret_cast<HWND>(lp), HIWORD(wp));
            }
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (HIWORD(wp) == EN_CHANGE || HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK) return 0;
            if (id == IDOK) {
                if (self->onOk && !self->onOk()) return 0;
                self->ok_ = true;
                self->done_ = true;
            } else if (id == IDCANCEL) {
                self->done_ = true;
            } else if (self->onCommand && HIWORD(wp) == BN_CLICKED) {
                self->onCommand(id);
            }
            return 0;
        }
        case WM_CLOSE:
            self->done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

private:
    HWND owner_;
    HWND hwnd_ = nullptr;
    HWND firstFocus_ = nullptr;
    int dpi_;
    HFONT font_;
    HFONT boldFont_ = nullptr;
    HBRUSH bg_;
    HBRUSH inputBg_;
    std::set<HWND> muted_;
    std::vector<std::pair<HWND, RECT>> edits_;  // edit and the frame drawn around it
    bool done_ = false;
    bool ok_ = false;
};

void RegisterDialogClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = Dialog::Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AsmDialog";
    RegisterClassW(&wc);
    registered = true;
}

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}

std::wstring BrowseExe(HWND owner, const std::wstring& current) {
    wchar_t buf[MAX_PATH] = L"";
    wcsncpy_s(buf, current.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = English() ? L"Programs (*.exe)\0*.exe\0All files\0*.*\0" : L"Программы (*.exe)\0*.exe\0Все файлы\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

int PopupChoice(HWND anchor, const std::vector<std::wstring>& items, int selected) {
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); i++) {
        AppendMenuW(menu, MF_STRING | (static_cast<int>(i) == selected ? MF_CHECKED : 0), i + 1, items[i].c_str());
    }
    RECT r;
    GetWindowRect(anchor, &r);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, anchor, nullptr);
    DestroyMenu(menu);
    return cmd - 1;
}

std::wstring LinesToText(const std::vector<std::wstring>& lines) {
    return Join(lines, L"\r\n");
}
}

std::optional<std::wstring> InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt,
                                     const std::wstring& initial) {
    RegisterDialogClass();
    int lines = static_cast<int>(std::count(prompt.begin(), prompt.end(), L'\n')) + 1 + (prompt.size() > 60 ? 1 : 0);
    int ph = 20 * lines;
    Dialog d(owner, title, 460, 104 + ph);
    d.Label(prompt, 16, 14, 428, ph);
    HWND edit = d.Edit(initial, 16, 18 + ph, 428, 26);
    d.Button(IDOK, L"OK", 268, 60 + ph, 84, 28, FB_PRIMARY);
    d.Button(IDCANCEL, Tr(L"Отмена"), 360, 60 + ph, 84, 28);
    std::wstring result;
    d.onOk = [&] {
        result = GetText(edit);
        return true;
    };
    if (!d.Run()) return std::nullopt;
    return result;
}

void ShowMessage(HWND owner, const std::wstring& text, bool error) {
    MessageBoxW(owner, text.c_str(), L"AsmEditor", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

bool ShowSettingsDialog(HWND owner, Settings& s) {
    RegisterDialogClass();
    // Two columns: tools on the left, editor and run options on the right.
    const int W = 1000, H = 630;
    const int X0 = 16, F = 150, FW = 480 - F;
    const int X1 = 516, F1 = X1 + 150, FW1 = W - 16 - F1;
    Dialog d(owner, Tr(L"Настройки"), W, H);
    int y = 12;
    d.Label(Tr(L"Ассемблер"), X0, y, 300, 20, false, true);
    y += 28;
    d.Label(Tr(L"Путь к nasm.exe:"), X0, y + 4, 130, 20);
    HWND nasmPath = d.Edit(s.nasmPath, F, y, FW - 44, 26);
    d.Button(kBrowseNasm, L"…", F + FW - 38, y, 38, 26);
    y += 34;
    d.Label(Tr(L"Аргументы NASM:"), X0, y + 4, 130, 20);
    HWND nasmArgs = d.Edit(s.nasmArgs.empty() ? Toolchain::kDefaultNasmArgs : s.nasmArgs, F, y, FW, 26);
    y += 30;
    d.Label(Tr(L"-f win64 -g -F cv8 (64 бит) или -f win32 -g -F cv8 (32 бит)."), F, y, FW, 18, true);
    y += 26;
    d.Label(L"Release:", X0, y + 4, 130, 20);
    HWND releaseNasm = d.Edit(s.releaseNasmArgs, F, y, FW, 26);
    y += 30;
    d.Label(Tr(L"Пусто — те же аргументы без -g и -F."), F, y, FW, 18, true);
    y += 28;

    d.Label(Tr(L"Линкер"), X0, y, 300, 20, false, true);
    y += 28;
    d.Label(Tr(L"Шаблон:"), X0, y + 4, 130, 20);
    std::wstring presetId = s.linkerPreset.empty() ? L"CUSTOM" : s.linkerPreset;
    HWND preset = d.Button(kPreset, std::wstring(Tr(FindPreset(presetId)->title)) + L"  ▾", F, y, FW, 26);
    y += 34;
    d.Label(Tr(L"Путь к линкеру:"), X0, y + 4, 130, 20);
    HWND linkerPath = d.Edit(s.linkerPath, F, y, FW - 44, 26);
    d.Button(kBrowseLinker, L"…", F + FW - 38, y, 38, 26);
    y += 34;
    d.Label(Tr(L"Аргументы:"), X0, y + 4, 130, 20);
    HWND linkerArgs = d.Edit(s.linkerArgs, F, y, FW, 58, true);
    y += 62;
    d.Label(Tr(L"Подстановки: ${obj}, ${exe}, ${name}, ${dir}."), F, y, FW, 18, true);
    y += 24;
    d.Label(L"Release:", X0, y + 4, 130, 20);
    HWND releaseLinker = d.Edit(s.releaseLinkerArgs, F, y, FW, 26);
    y += 30;
    d.Label(Tr(L"Пусто — те же аргументы без /debug."), F, y, FW, 18, true);
    y += 24;
    d.Label(Tr(L"Папки библиотек:"), X0, y + 4, 130, 20);
    HWND libPaths = d.Edit(LinesToText(s.libPaths), F, y, FW, 66, true);
    y += 70;
    d.Label(Tr(L"По одной на строку; для link.exe — /LIBPATH."), F, y, FW, 18, true);

    y = 12;
    d.Label(Tr(L"Запуск"), X1, y, 300, 20, false, true);
    y += 28;
    d.Label(Tr(L"Кодировка вывода:"), X1, y + 4, 140, 20);
    std::wstring encoding = s.programEncoding;
    HWND enc = d.Button(kEncoding, encoding + L"  ▾", F1, y, 160, 26);
    y += 36;
    HWND external = d.Button(kExternal, Tr(L"Запускать программу во внешнем окне консоли"), X1, y, W - X1 - 16, 22, FB_CHECK);
    SendMessageW(external, BM_SETCHECK, s.externalConsole ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 36;

    d.Label(Tr(L"Редактор"), X1, y, 300, 20, false, true);
    y += 28;
    d.Label(Tr(L"Шрифт:"), X1, y + 4, 140, 20);
    HWND font = d.Edit(std::to_wstring(s.fontSize), F1, y, 60, 26);
    y += 34;
    d.Label(Tr(L"Тема:"), X1, y + 4, 140, 20);
    std::wstring theme = s.theme;
    auto themeTitle = [&] {
        for (const Theme::Info& t : Theme::All()) {
            if (theme == t.id) return std::wstring(Tr(t.title));
        }
        return std::wstring(Tr(Theme::All()[0].title));
    };
    HWND themeButton = d.Button(kTheme, themeTitle() + L"  ▾", F1, y, 200, 26);
    y += 34;
    d.Label(Tr(L"Язык интерфейса:"), X1, y + 4, 140, 20);
    std::wstring language = s.language;
    HWND langButton = d.Button(kLanguage, (language == L"en" ? L"English" : L"Русский") + std::wstring(L"  ▾"), F1, y, 200, 26);
    y += 34;
    d.Label(Tr(L"Форматирование:"), X1, y + 4, 140, 20);
    d.Label(Tr(L"мнемоника"), F1, y + 4, 80, 20, true);
    HWND fIndent = d.Edit(std::to_wstring(s.formatIndent), F1 + 80, y, 44, 26);
    d.Label(Tr(L"операнды"), F1 + 134, y + 4, 70, 20, true);
    HWND fOperands = d.Edit(std::to_wstring(s.formatOperands), F1 + 204, y, 44, 26);
    d.Label(Tr(L"комментарий"), F1 + 258, y + 4, 90, 20, true);
    HWND fComments = d.Edit(std::to_wstring(s.formatComments), F1 + 348, y, 44, 26);
    y += 30;
    d.Label(Tr(L"Номера столбцов (с нуля) для Ctrl+Alt+L."), F1, y, FW1, 18, true);
    y += 28;
    HWND live = d.Button(kLive, Tr(L"Проверять ошибки во время набора (NASM в фоне)"), X1, y, W - X1 - 16, 22, FB_CHECK);
    SendMessageW(live, BM_SETCHECK, s.liveCheck ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 28;
    HWND stack = d.Button(kStackCheck, Tr(L"Предупреждать о невыровненном стеке перед call (Win64)"), X1, y, W - X1 - 16, 22, FB_CHECK);
    SendMessageW(stack, BM_SETCHECK, s.stackCheck ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 28;
    HWND autosave = d.Button(kAutosave, Tr(L"Сохранять файлы при переключении в другое окно"), X1, y, W - X1 - 16, 22, FB_CHECK);
    SendMessageW(autosave, BM_SETCHECK, s.autosave ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 28;
    HWND formatOnSave = d.Button(kFormatOnSave, Tr(L"Форматировать код при сохранении (Ctrl+Alt+L)"), X1, y, W - X1 - 16, 22,
                                 FB_CHECK);
    SendMessageW(formatOnSave, BM_SETCHECK, s.formatOnSave ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 28;
    HWND log = d.Button(kLog, Tr(L"Вести журнал работы (asmeditor.log)"), X1, y, W - X1 - 16, 22, FB_CHECK);
    SendMessageW(log, BM_SETCHECK, s.log ? BST_CHECKED : BST_UNCHECKED, 0);

    int by = H - 44;
    d.Button(kDetect, Tr(L"Автоопределение"), X0, by, 150, 30);
    d.Button(IDOK, Tr(L"Сохранить"), W - 16 - 110 - 8 - 110, by, 110, 30, FB_PRIMARY);
    d.Button(IDCANCEL, Tr(L"Отмена"), W - 16 - 110, by, 110, 30);

    auto setPreset = [&](const std::wstring& id) {
        presetId = id;
        SetWindowTextW(preset, (std::wstring(Tr(FindPreset(id)->title)) + L"  ▾").c_str());
    };
    auto fillMsvc = [&](const MsvcInfo& m) {
        SetWindowTextW(linkerPath, m.link.c_str());
        std::vector<std::wstring> libs;
        for (const fs::path& p : m.libPaths) libs.push_back(p.wstring());
        SetWindowTextW(libPaths, LinesToText(libs).c_str());
    };

    d.onCommand = [&](int id) {
        switch (id) {
        case kBrowseNasm:
            if (auto p = BrowseExe(d.hwnd(), GetText(nasmPath)); !p.empty()) SetWindowTextW(nasmPath, p.c_str());
            break;
        case kBrowseLinker:
            if (auto p = BrowseExe(d.hwnd(), GetText(linkerPath)); !p.empty()) SetWindowTextW(linkerPath, p.c_str());
            break;
        case kPreset: {
            std::vector<std::wstring> titles;
            int current = 0;
            for (int i = 0; i < kLinkerPresetCount; i++) {
                titles.push_back(Tr(kLinkerPresets[i].title));
                if (presetId == kLinkerPresets[i].id) current = i;
            }
            int choice = PopupChoice(preset, titles, current);
            if (choice < 0) break;
            const LinkerPreset& p = kLinkerPresets[choice];
            setPreset(p.id);
            if (!p.args) break;
            SetWindowTextW(linkerArgs, p.args);
            std::wstring cur = Lower(GetText(linkerPath));
            bool gcc = std::wstring(p.id) == L"GCC";
            bool x86 = std::wstring(p.id).find(L"X86") != std::wstring::npos;
            if (gcc && cur.find(L"gcc.exe") == std::wstring::npos) {
                if (auto g = Toolchain::FindOnPath(L"gcc.exe")) SetWindowTextW(linkerPath, g->c_str());
                SetWindowTextW(libPaths, L"");
            } else if (!gcc) {
                if (auto m = Toolchain::FindMsvc(x86)) fillMsvc(*m);
                if (x86 && Trim(GetText(nasmArgs)).find(L"win64") != std::wstring::npos) {
                    SetWindowTextW(nasmArgs, L"-f win32 -g -F cv8");
                } else if (!x86 && Trim(GetText(nasmArgs)).find(L"win32") != std::wstring::npos) {
                    SetWindowTextW(nasmArgs, Toolchain::kDefaultNasmArgs);
                }
            }
            break;
        }
        case kEncoding: {
            std::vector<std::wstring> options{L"UTF-8", L"cp866", L"windows-1251"};
            int current = static_cast<int>(std::find(options.begin(), options.end(), encoding) - options.begin());
            int choice = PopupChoice(enc, options, current);
            if (choice >= 0) {
                encoding = options[choice];
                SetWindowTextW(enc, (encoding + L"  ▾").c_str());
            }
            break;
        }
        case kTheme: {
            std::vector<std::wstring> options;
            int current = 0;
            for (size_t i = 0; i < Theme::All().size(); i++) {
                options.push_back(Tr(Theme::All()[i].title));
                if (theme == Theme::All()[i].id) current = static_cast<int>(i);
            }
            int choice = PopupChoice(themeButton, options, current);
            if (choice >= 0) {
                theme = Theme::All()[choice].id;
                SetWindowTextW(themeButton, (themeTitle() + L"  ▾").c_str());
            }
            break;
        }
        case kLanguage: {
            int choice = PopupChoice(langButton, {L"Русский", L"English"}, language == L"en" ? 1 : 0);
            if (choice >= 0) {
                language = choice ? L"en" : L"ru";
                SetWindowTextW(langButton, (std::wstring(choice ? L"English" : L"Русский") + L"  ▾").c_str());
            }
            break;
        }
        case kDetect: {
            if (auto n = Toolchain::FindNasm()) SetWindowTextW(nasmPath, n->c_str());
            if (Trim(GetText(nasmArgs)).empty()) SetWindowTextW(nasmArgs, Toolchain::kDefaultNasmArgs);
            if (auto m = Toolchain::FindMsvc()) {
                setPreset(L"MSVC_CRT");
                SetWindowTextW(linkerArgs, FindPreset(L"MSVC_CRT")->args);
                fillMsvc(*m);
            } else if (auto g = Toolchain::FindOnPath(L"gcc.exe")) {
                setPreset(L"GCC");
                SetWindowTextW(linkerArgs, FindPreset(L"GCC")->args);
                SetWindowTextW(linkerPath, g->c_str());
                SetWindowTextW(libPaths, L"");
            } else {
                ShowMessage(d.hwnd(), Tr(L"Не найден ни MSVC link.exe, ни MinGW gcc."), false);
            }
            break;
        }
        }
    };

    auto checked = [](HWND b) { return SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    d.onOk = [&] {
        s.nasmPath = Trim(GetText(nasmPath));
        s.nasmArgs = Trim(GetText(nasmArgs));
        s.releaseNasmArgs = Trim(GetText(releaseNasm));
        s.releaseLinkerArgs = Trim(GetText(releaseLinker));
        s.linkerPath = Trim(GetText(linkerPath));
        std::wstring args;
        for (const std::wstring& part : Split(GetText(linkerArgs), L'\n')) args += (args.empty() ? L"" : L" ") + part;
        const LinkerPreset* p = FindPreset(presetId);
        s.linkerPreset = (p->args && args != p->args) ? L"CUSTOM" : presetId;
        s.linkerArgs = args;
        s.libPaths = Split(GetText(libPaths), L'\n');
        s.programEncoding = encoding;
        s.externalConsole = checked(external);
        s.liveCheck = checked(live);
        s.stackCheck = checked(stack);
        s.autosave = checked(autosave);
        s.formatOnSave = checked(formatOnSave);
        s.log = checked(log);
        s.theme = theme;
        s.language = language;
        int size = _wtoi(GetText(font).c_str());
        if (size >= 6 && size <= 40) s.fontSize = size;
        int indent = _wtoi(GetText(fIndent).c_str()), operands = _wtoi(GetText(fOperands).c_str()),
            comments = _wtoi(GetText(fComments).c_str());
        if (indent >= 0 && operands > indent && comments > operands) {
            s.formatIndent = indent;
            s.formatOperands = operands;
            s.formatComments = comments;
        }
        s.Save();
        return true;
    };
    return d.Run();
}

// ---- list picker --------------------------------------------------------------------------------------------------

namespace {
// Every character of the filter appears in the text in order ("prj" matches "Перейти к файлу"? no: "pf" — "Палитра…").
// Words that start with the filter rank first.
int MatchScore(const std::wstring& text, const std::wstring& filter) {
    if (filter.empty()) return 1;
    std::wstring t = Lower(text), f = Lower(filter);
    size_t at = t.find(f);
    if (at != std::wstring::npos) return at == 0 ? 1000 : (t[at - 1] == L' ' ? 800 : 600);
    size_t i = 0;
    for (wchar_t c : t) {
        if (i < f.size() && c == f[i]) i++;
    }
    return i == f.size() ? 100 : 0;
}

LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR list) {
    if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT)) {
        HWND l = reinterpret_cast<HWND>(list);
        int count = static_cast<int>(SendMessageW(l, LB_GETCOUNT, 0, 0));
        int sel = static_cast<int>(SendMessageW(l, LB_GETCURSEL, 0, 0));
        int step = wp == VK_UP ? -1 : wp == VK_DOWN ? 1 : wp == VK_PRIOR ? -10 : 10;
        if (count > 0) SendMessageW(l, LB_SETCURSEL, std::clamp(sel + step, 0, count - 1), 0);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
}

std::optional<int> PickFromList(HWND owner, const std::wstring& title, const std::vector<std::wstring>& items,
                                const std::wstring& placeholder) {
    RegisterDialogClass();
    Dialog d(owner, title, 640, 440);
    HWND filter = d.Edit(L"", 16, 14, 608, 28);
    SendMessageW(filter, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(placeholder.c_str()));
    HWND list = d.List(16, 52, 608, 330);
    d.Button(IDOK, L"OK", 432, 396, 92, 30, FB_PRIMARY);
    d.Button(IDCANCEL, Tr(L"Отмена"), 532, 396, 92, 30);
    SetWindowSubclass(filter, FilterEditProc, 1, reinterpret_cast<DWORD_PTR>(list));
    std::vector<int> shown;
    auto refill = [&] {
        std::wstring f = Trim(GetText(filter));
        std::vector<std::pair<int, int>> ranked;  // score, index
        for (size_t i = 0; i < items.size(); i++) {
            std::wstring text = items[i].substr(0, items[i].find(L'\t'));
            int score = MatchScore(text, f);
            if (score) ranked.push_back({-score, static_cast<int>(i)});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        shown.clear();
        for (auto [score, i] : ranked) {
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i].c_str()));
            shown.push_back(i);
        }
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
    };
    refill();
    int result = -1;
    d.onControl = [&](HWND control, int code) {
        if (control == filter && code == EN_CHANGE) refill();
        if (control == list && code == LBN_DBLCLK) d.Finish(true);
    };
    d.onOk = [&] {
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= static_cast<int>(shown.size())) return false;
        result = shown[sel];
        return true;
    };
    if (!d.Run() || result < 0) return std::nullopt;
    return result;
}

// ---- find in files ------------------------------------------------------------------------------------------------

std::optional<FindInFilesRequest> ShowFindInFilesDialog(HWND owner, const FindInFilesRequest& initial) {
    RegisterDialogClass();
    Dialog d(owner, Tr(L"Найти в файлах"), 560, 250);
    d.Label(Tr(L"Найти:"), 16, 18, 110, 20);
    HWND what = d.Edit(initial.what, 130, 14, 414, 26);
    d.Label(Tr(L"Заменить на:"), 16, 54, 110, 20);
    HWND with = d.Edit(initial.replaceWith, 130, 50, 414, 26);
    HWND matchCase = d.Button(kMatchCase, Tr(L"С учётом регистра"), 130, 90, 200, 22, FB_CHECK);
    HWND wholeWord = d.Button(kWholeWord, Tr(L"Слово целиком"), 340, 90, 200, 22, FB_CHECK);
    HWND regex = d.Button(kRegex, Tr(L"Регулярное выражение"), 130, 118, 260, 22, FB_CHECK);
    SendMessageW(matchCase, BM_SETCHECK, initial.matchCase ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(wholeWord, BM_SETCHECK, initial.wholeWord ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(regex, BM_SETCHECK, initial.regex ? BST_CHECKED : BST_UNCHECKED, 0);
    d.Label(Tr(L"Ищется в открытых вкладках и текстовых файлах проекта. Замена правит файлы во вкладках без сохранения."),
            16, 152, 528, 36, true);
    d.Button(kReplaceAll, Tr(L"Заменить всё"), 240, 204, 120, 30);
    d.Button(IDOK, Tr(L"Найти"), 368, 204, 84, 30, FB_PRIMARY);
    d.Button(IDCANCEL, Tr(L"Отмена"), 460, 204, 84, 30);
    FindInFilesRequest r = initial;
    auto read = [&] {
        r.what = GetText(what);
        r.replaceWith = GetText(with);
        r.matchCase = SendMessageW(matchCase, BM_GETCHECK, 0, 0) == BST_CHECKED;
        r.wholeWord = SendMessageW(wholeWord, BM_GETCHECK, 0, 0) == BST_CHECKED;
        r.regex = SendMessageW(regex, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };
    d.onCommand = [&](int id) {
        if (id == kReplaceAll) {
            r.replace = true;
            d.Finish(true);
        }
    };
    d.onOk = [&] {
        read();
        return !r.what.empty();
    };
    r.replace = false;
    if (!d.Run()) return std::nullopt;
    return r;
}

// ---- run configuration --------------------------------------------------------------------------------------------

bool ShowRunConfigDialog(HWND owner, RunConfig& c) {
    RegisterDialogClass();
    const int W = 640, F = 190, FW = W - F - 16;
    Dialog d(owner, Tr(L"Конфигурация запуска"), W, 560);
    int y = 14;
    d.Label(Tr(L"Аргументы программы:"), 16, y + 4, 170, 20);
    HWND args = d.Edit(c.args, F, y, FW, 26);
    y += 36;
    d.Label(Tr(L"Рабочая папка:"), 16, y + 4, 170, 20);
    HWND dir = d.Edit(c.workDir, F, y, FW, 26);
    y += 30;
    d.Label(Tr(L"Относительно проекта; пусто — папка проекта."), F, y, FW, 18, true);
    y += 26;
    d.Label(Tr(L"Переменные окружения:"), 16, y + 4, 170, 20);
    HWND env = d.Edit(c.env, F, y, FW, 58, true);
    y += 62;
    d.Label(Tr(L"По одной: ИМЯ=значение."), F, y, FW, 18, true);
    y += 26;
    d.Label(Tr(L"Файл для stdin:"), 16, y + 4, 170, 20);
    HWND stdinFile = d.Edit(c.stdinFile, F, y, FW, 26);
    y += 30;
    d.Label(Tr(L"Его содержимое вводится в программу при запуске (потом — Ctrl+Z)."), F, y, FW, 18, true);
    y += 30;
    d.Label(Tr(L"Тип результата:"), 16, y + 4, 170, 20);
    std::wstring output = c.output;
    auto outputTitle = [&] {
        return std::wstring(output == L"dll" ? Tr(L"динамическая библиотека (.dll)")
                            : output == L"lib" ? Tr(L"статическая библиотека (.lib)")
                                               : Tr(L"программа (.exe)")) + L"  ▾";
    };
    HWND outButton = d.Button(kOutput, outputTitle(), F, y, FW, 26);
    y += 34;
    d.Label(Tr(L"Платформа:"), 16, y + 4, 170, 20);
    std::wstring platform = c.platform;
    auto platformTitle = [&] {
        return std::wstring(platform == L"wsl" ? Tr(L"Linux через WSL (-f elf64, ld/gcc, gdb)") : L"Windows") + L"  ▾";
    };
    HWND platButton = d.Button(kPlatform, platformTitle(), F, y, FW, 26);
    y += 40;
    d.Label(Tr(L"Перед сборкой:"), 16, y + 4, 170, 20);
    HWND pre = d.Edit(c.preBuild, F, y, FW, 50, true);
    y += 56;
    d.Label(Tr(L"После сборки:"), 16, y + 4, 170, 20);
    HWND post = d.Edit(c.postBuild, F, y, FW, 50, true);
    y += 54;
    d.Label(Tr(L"Команды cmd.exe по одной на строку, в папке проекта (например, copy data.txt build\\)."), F, y, FW, 18, true);
    d.Button(IDOK, Tr(L"Сохранить"), W - 16 - 96 - 8 - 96, 560 - 44, 96, 30, FB_PRIMARY);
    d.Button(IDCANCEL, Tr(L"Отмена"), W - 16 - 96, 560 - 44, 96, 30);
    d.onCommand = [&](int id) {
        if (id == kOutput) {
            std::vector<std::wstring> ids{L"exe", L"dll", L"lib"};
            int current = static_cast<int>(std::find(ids.begin(), ids.end(), output) - ids.begin());
            int choice = PopupChoice(outButton, {Tr(L"программа (.exe)"), Tr(L"динамическая библиотека (.dll)"),
                                                 Tr(L"статическая библиотека (.lib)")}, current);
            if (choice >= 0) {
                output = ids[choice];
                SetWindowTextW(outButton, outputTitle().c_str());
            }
        } else if (id == kPlatform) {
            int choice = PopupChoice(platButton, {L"Windows", Tr(L"Linux через WSL (-f elf64, ld/gcc, gdb)")},
                                     platform == L"wsl" ? 1 : 0);
            if (choice >= 0) {
                platform = choice ? L"wsl" : L"windows";
                SetWindowTextW(platButton, platformTitle().c_str());
            }
        }
    };
    d.onOk = [&] {
        c.args = Trim(GetText(args));
        c.workDir = Trim(GetText(dir));
        c.env = Trim(GetText(env));
        c.stdinFile = Trim(GetText(stdinFile));
        c.preBuild = Trim(GetText(pre));
        c.postBuild = Trim(GetText(post));
        c.output = output;
        c.platform = platform;
        for (std::wstring* text : {&c.env, &c.preBuild, &c.postBuild}) {
            text->erase(std::remove(text->begin(), text->end(), L'\r'), text->end());
        }
        return true;
    };
    return d.Run();
}

// ---- shortcuts ----------------------------------------------------------------------------------------------------

bool ShowKeysDialog(HWND owner, Settings& s, const std::vector<std::pair<std::wstring, std::wstring>>& commands) {
    RegisterDialogClass();
    Dialog d(owner, Tr(L"Горячие клавиши"), 640, 480);
    HWND list = d.List(16, 14, 608, 360);
    d.Label(Tr(L"Двойной клик — изменить. Формат: Ctrl+Shift+K, Alt+F9, F5; два сочетания — через « / »."),
            16, 382, 608, 36, true);
    d.Button(kResetKeys, Tr(L"Сбросить все"), 16, 436, 130, 30);
    d.Button(IDOK, Tr(L"Сохранить"), 432, 436, 92, 30, FB_PRIMARY);
    d.Button(IDCANCEL, Tr(L"Отмена"), 532, 436, 92, 30);
    std::map<std::wstring, std::wstring> keys = s.keys;
    auto current = [&](size_t i) {
        auto it = keys.find(commands[i].first);
        return it != keys.end() ? it->second : commands[i].second;
    };
    auto refill = [&] {
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < commands.size(); i++) {
            std::wstring k = current(i);
            std::wstring row = std::wstring(Tr(commands[i].first.c_str())) + L"\t" + (k.empty() ? L"—" : k);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        }
        SendMessageW(list, LB_SETCURSEL, std::max(0, sel), 0);
    };
    refill();
    auto edit = [&] {
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel < 0) return;
        auto text = InputBox(d.hwnd(), Tr(commands[sel].first.c_str()), Tr(L"Сочетание клавиш (пусто — без сочетания):"),
                             current(sel));
        if (!text) return;
        std::wstring k = Trim(*text);
        if (k == commands[sel].second) keys.erase(commands[sel].first);
        else keys[commands[sel].first] = k;
        refill();
    };
    d.onControl = [&](HWND control, int code) {
        if (control == list && code == LBN_DBLCLK) edit();
    };
    d.onCommand = [&](int id) {
        if (id == kResetKeys) {
            keys.clear();
            refill();
        }
    };
    d.onOk = [&] {
        // Enter in the list edits the row; the Save button saves.
        if (GetFocus() == list) {
            edit();
            return false;
        }
        s.keys = keys;
        return true;
    };
    return d.Run();
}
