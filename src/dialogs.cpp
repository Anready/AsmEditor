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

// Modal dark dialog built from plain child windows (no resource files).
class Dialog {
public:
    std::function<bool()> onOk;
    std::function<void(int)> onCommand;

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
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, Theme::Bright);
            SetBkColor(dc, Theme::InputBg);
            return reinterpret_cast<LRESULT>(self->inputBg_);
        }
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
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
    ofn.lpstrFilter = L"Программы (*.exe)\0*.exe\0Все файлы\0*.*\0";
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
    Dialog d(owner, title, 420, 124);
    d.Label(prompt, 16, 14, 388, 20);
    HWND edit = d.Edit(initial, 16, 38, 388, 26);
    d.Button(IDOK, L"OK", 228, 80, 84, 28, FB_PRIMARY);
    d.Button(IDCANCEL, L"Отмена", 320, 80, 84, 28);
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
    const int W = 660;
    const int X0 = 16, F = 170, FW = W - F - 16;
    Dialog d(owner, L"Настройки", W, 610);
    int y = 12;
    d.Label(L"Ассемблер", X0, y, 300, 20, false, true);
    y += 28;
    d.Label(L"Путь к nasm.exe:", X0, y + 4, 150, 20);
    HWND nasmPath = d.Edit(s.nasmPath, F, y, FW - 44, 26);
    d.Button(kBrowseNasm, L"…", F + FW - 38, y, 38, 26);
    y += 34;
    d.Label(L"Аргументы NASM:", X0, y + 4, 150, 20);
    HWND nasmArgs = d.Edit(s.nasmArgs.empty() ? Toolchain::kDefaultNasmArgs : s.nasmArgs, F, y, FW, 26);
    y += 30;
    d.Label(L"Например: -f win64 -g -F cv8 (64-бит) или -f win32 (32-бит).", F, y, FW, 18, true);
    y += 30;

    d.Label(L"Линкер", X0, y, 300, 20, false, true);
    y += 28;
    d.Label(L"Шаблон:", X0, y + 4, 150, 20);
    std::wstring presetId = s.linkerPreset.empty() ? L"CUSTOM" : s.linkerPreset;
    HWND preset = d.Button(kPreset, std::wstring(FindPreset(presetId)->title) + L"  ▾", F, y, FW, 26);
    y += 34;
    d.Label(L"Путь к линкеру:", X0, y + 4, 150, 20);
    HWND linkerPath = d.Edit(s.linkerPath, F, y, FW - 44, 26);
    d.Button(kBrowseLinker, L"…", F + FW - 38, y, 38, 26);
    y += 34;
    d.Label(L"Аргументы:", X0, y + 4, 150, 20);
    HWND linkerArgs = d.Edit(s.linkerArgs, F, y, FW, 58, true);
    y += 62;
    d.Label(L"Подстановки: ${obj}, ${exe}, ${name}, ${dir}.", F, y, FW, 18, true);
    y += 24;
    d.Label(L"Папки библиотек:", X0, y + 4, 150, 20);
    HWND libPaths = d.Edit(LinesToText(s.libPaths), F, y, FW, 74, true);
    y += 78;
    d.Label(L"По одной на строку; для link.exe передаются как /LIBPATH.", F, y, FW, 18, true);
    y += 30;

    d.Label(L"Запуск и редактор", X0, y, 300, 20, false, true);
    y += 28;
    d.Label(L"Кодировка вывода:", X0, y + 4, 150, 20);
    std::wstring encoding = s.programEncoding;
    HWND enc = d.Button(kEncoding, encoding + L"  ▾", F, y, 160, 26);
    d.Label(L"Шрифт:", F + 190, y + 4, 60, 20);
    HWND font = d.Edit(std::to_wstring(s.fontSize), F + 250, y, 60, 26);
    y += 36;
    HWND external = d.Button(kExternal, L"Запускать программу во внешнем окне консоли", F, y, FW, 22, FB_CHECK);
    SendMessageW(external, BM_SETCHECK, s.externalConsole ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 28;
    HWND live = d.Button(kLive, L"Проверять ошибки во время набора (NASM в фоне)", F, y, FW, 22, FB_CHECK);
    SendMessageW(live, BM_SETCHECK, s.liveCheck ? BST_CHECKED : BST_UNCHECKED, 0);

    int by = 610 - 44;
    d.Button(kDetect, L"Автоопределение", X0, by, 150, 30);
    d.Button(IDOK, L"Сохранить", W - 16 - 96 - 8 - 96, by, 96, 30, FB_PRIMARY);
    d.Button(IDCANCEL, L"Отмена", W - 16 - 96, by, 96, 30);

    auto setPreset = [&](const std::wstring& id) {
        presetId = id;
        SetWindowTextW(preset, (std::wstring(FindPreset(id)->title) + L"  ▾").c_str());
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
                titles.push_back(kLinkerPresets[i].title);
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
            if (gcc && cur.find(L"gcc.exe") == std::wstring::npos) {
                if (auto g = Toolchain::FindOnPath(L"gcc.exe")) SetWindowTextW(linkerPath, g->c_str());
                SetWindowTextW(libPaths, L"");
            } else if (!gcc && cur.find(L"link.exe") == std::wstring::npos) {
                if (auto m = Toolchain::FindMsvc()) fillMsvc(*m);
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
                ShowMessage(d.hwnd(), L"Не найден ни MSVC link.exe, ни MinGW gcc.", false);
            }
            break;
        }
        }
    };

    d.onOk = [&] {
        s.nasmPath = Trim(GetText(nasmPath));
        s.nasmArgs = Trim(GetText(nasmArgs));
        s.linkerPath = Trim(GetText(linkerPath));
        std::wstring args;
        for (const std::wstring& part : Split(GetText(linkerArgs), L'\n')) args += (args.empty() ? L"" : L" ") + part;
        const LinkerPreset* p = FindPreset(presetId);
        s.linkerPreset = (p->args && args != p->args) ? L"CUSTOM" : presetId;
        s.linkerArgs = args;
        s.libPaths = Split(GetText(libPaths), L'\n');
        s.programEncoding = encoding;
        s.externalConsole = SendMessageW(external, BM_GETCHECK, 0, 0) == BST_CHECKED;
        s.liveCheck = SendMessageW(live, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int size = _wtoi(GetText(font).c_str());
        if (size >= 6 && size <= 40) s.fontSize = size;
        s.Save();
        return true;
    };
    return d.Run();
}
