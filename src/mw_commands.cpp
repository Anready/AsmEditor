#include "mw_internal.h"
#include "version.h"

#include <winhttp.h>

// ---- command table ------------------------------------------------------------------------------------------------

// Every command with its default shortcut. "~" marks a key handled by the editor control itself (shown in menus,
// not registered as an accelerator); "a / b" gives two shortcuts.
const std::vector<MainWindow::CommandDef>& MainWindow::Commands() {
    static const std::vector<CommandDef> list = {
        {ID_NEW, L"Новый файл", L"Ctrl+N"},
        {ID_NEW_PROJECT, L"Новый проект…", L""},
        {ID_OPEN_FILE, L"Открыть файл…", L"Ctrl+O"},
        {ID_OPEN_FOLDER, L"Открыть папку…", L"Ctrl+Shift+O"},
        {ID_QUICK_OPEN, L"Перейти к файлу…", L"Ctrl+P"},
        {ID_SAVE, L"Сохранить", L"Ctrl+S"},
        {ID_SAVE_AS, L"Сохранить как…", L"Ctrl+Shift+S"},
        {ID_SAVE_ALL, L"Сохранить всё", L""},
        {ID_CLOSE_TAB, L"Закрыть вкладку", L"Ctrl+W"},
        {ID_CLOSE_OTHERS, L"Закрыть другие вкладки", L""},
        {ID_CLOSE_ALL, L"Закрыть все вкладки", L""},
        {ID_CLOSE_PROJECT, L"Закрыть проект", L""},
        {ID_EXIT, L"Выход", L""},
        {ID_UNDO, L"Отменить", L"~Ctrl+Z"},
        {ID_REDO, L"Повторить", L"~Ctrl+Y"},
        {ID_FIND, L"Найти…", L"Ctrl+F"},
        {ID_REPLACE, L"Заменить…", L"Ctrl+H"},
        {ID_FIND_NEXT, L"Найти далее", L"F3"},
        {ID_FIND_PREV, L"Найти назад", L"Shift+F3"},
        {ID_FIND_IN_FILES, L"Найти в файлах…", L"Ctrl+Shift+F"},
        {ID_GOTO, L"Перейти к строке…", L"Ctrl+G"},
        {ID_COMPLETE, L"Автодополнение", L"Ctrl+Space"},
        {ID_SNIPPET, L"Вставить шаблон кода…", L"Ctrl+J"},
        {ID_SHOW_HINT, L"Подсказка по инструкции", L"Ctrl+Shift+Space"},
        {ID_GOTO_DEF, L"Перейти к определению", L"F12"},
        {ID_FIND_USAGES, L"Найти использования", L"Shift+F12"},
        {ID_RENAME, L"Переименовать…", L"F2"},
        {ID_NAV_BACK, L"Назад", L"Alt+Left"},
        {ID_NAV_FORWARD, L"Вперёд", L"Alt+Right"},
        {ID_COMMENT, L"Закомментировать / раскомментировать", L"Ctrl+/ / Ctrl+Divide"},
        {ID_DUPLICATE, L"Дублировать строку", L"~Ctrl+D"},
        {ID_FORMAT, L"Форматировать код", L"Ctrl+Alt+L"},
        {ID_QUICK_FIX, L"Быстрое исправление…", L"Alt+Enter"},
        {ID_ADD_CARET_UP, L"Добавить курсор выше", L"Ctrl+Alt+Up"},
        {ID_ADD_CARET_DOWN, L"Добавить курсор ниже", L"Ctrl+Alt+Down"},
        {ID_SELECT_NEXT, L"Выделить следующее вхождение", L"Alt+J"},
        {ID_SELECT_ALL_OCCURRENCES, L"Выделить все вхождения", L"Ctrl+Alt+Shift+J"},
        {ID_TOGGLE_FOLD, L"Свернуть / развернуть блок", L"Ctrl+M"},
        {ID_FOLD_ALL, L"Свернуть всё", L"Ctrl+Shift+-"},
        {ID_UNFOLD_ALL, L"Развернуть всё", L"Ctrl+Shift+="},
        {ID_COMMAND_PALETTE, L"Палитра команд…", L"Ctrl+Shift+P"},
        {ID_ZOOM_IN, L"Увеличить шрифт", L"Ctrl+= / Ctrl+Add"},
        {ID_ZOOM_OUT, L"Уменьшить шрифт", L"Ctrl+- / Ctrl+Subtract"},
        {ID_ZOOM_RESET, L"Сбросить размер шрифта", L"Ctrl+0"},
        {ID_NEXT_TAB, L"Следующая вкладка", L"Ctrl+Tab"},
        {ID_PREV_TAB, L"Предыдущая вкладка", L"Ctrl+Shift+Tab"},
        {ID_SPLIT, L"Открыть справа (разделить)", L"Ctrl+\\"},
        {ID_UNSPLIT, L"Убрать разделение", L""},
        {ID_PIN_TAB, L"Закрепить / открепить вкладку", L""},
        {ID_OUTLINE, L"Структура файла", L"Ctrl+F12"},
        {ID_DEBUG_PANEL, L"Панель отладки", L""},
        {ID_SHOW_OUTPUT, L"Вкладка «Вывод»", L"Ctrl+Shift+U"},
        {ID_SHOW_TERMINAL, L"Вкладка «Терминал»", L"Ctrl+`"},
        {ID_SHOW_PROBLEMS, L"Вкладка «Проблемы»", L"Ctrl+Shift+M"},
        {ID_LANGUAGE, L"English / Русский", L""},
        {ID_BUILD, L"Собрать", L"Ctrl+F9"},
        {ID_REBUILD, L"Пересобрать всё", L"Ctrl+Shift+F9"},
        {ID_RUN, L"Запустить без отладки", L"Ctrl+F5 / Shift+F10"},
        {ID_DEBUG, L"Отладка / продолжить", L"F5"},
        {ID_RUN_TO_CURSOR, L"Выполнить до курсора", L"Ctrl+F10"},
        {ID_SET_NEXT, L"Перенести выполнение сюда", L"Ctrl+Shift+F10"},
        {ID_PROFILE_RUN, L"Профилировать (сколько раз выполнилась каждая строка)", L""},
        {ID_PROFILE_SAMPLE, L"Профилировать по таймеру (быстро, без подсчёта строк)", L""},
        {ID_PROFILE_CLEAR, L"Убрать результаты профилирования", L""},
        {ID_STEP_OVER, L"Шаг с обходом", L"F10"},
        {ID_STEP_INTO, L"Шаг с заходом", L"F11"},
        {ID_STEP_OUT, L"Шаг с выходом", L"Shift+F11"},
        {ID_STEP_BACK, L"Шаг назад", L"Ctrl+Shift+F11"},
        {ID_PAUSE, L"Пауза", L""},
        {ID_STOP, L"Остановить", L"Shift+F5 / Ctrl+F2"},
        {ID_TOGGLE_BP, L"Точка останова", L"F9"},
        {ID_BP_CONDITION, L"Условие точки останова…", L"Alt+F9"},
        {ID_CLEAR_BPS, L"Удалить все точки останова", L""},
        {ID_ADD_WATCH, L"Добавить в наблюдение", L"Ctrl+Alt+W"},
        {ID_DATA_BP, L"Остановиться при изменении данных…", L""},
        {ID_RUN_CONFIG, L"Конфигурация запуска…", L""},
        {ID_CHOOSE_TARGET, L"Что запускать…", L""},
        {ID_PROFILE_DEBUG, L"Профиль Debug", L""},
        {ID_PROFILE_RELEASE, L"Профиль Release", L""},
        {ID_CLEAR_CONSOLE, L"Очистить вкладку", L""},
        {ID_RESET_MAIN, L"Сбросить главный файл проекта", L""},
        {ID_CHECK_SOLUTION, L"Проверить решение задачи", L"Ctrl+Shift+T"},
        {ID_NEW_EXERCISE, L"Новая учебная задача…", L""},
        {ID_LESSONS, L"Уроки…", L""},
        {ID_EXERCISE_HINT, L"Подсказка к задаче", L"Ctrl+Shift+H"},
        {ID_GROUP_SUMMARY, L"Сводка по группе (из файлов результатов)…", L""},
        {ID_EXPORT_RESULTS, L"Экспорт результатов задач…", L""},
        {ID_SETTINGS, L"Настройки…", L"Ctrl+Alt+S"},
        {ID_KEYS, L"Горячие клавиши…", L""},
        {ID_CHECK_UPDATES, L"Проверить обновления NASM и Scintilla", L""},
        {ID_OPEN_LOG, L"Открыть журнал работы", L""},
        {ID_ASSOCIATE, L"Открывать файлы .asm в AsmEditor", L""},
        {ID_ABOUT, L"О программе", L""},
    };
    return list;
}

std::wstring MainWindow::KeysFor(int id) const {
    for (const CommandDef& c : Commands()) {
        if (c.id != id) continue;
        auto it = settings_->keys.find(c.name);
        std::wstring keys = it != settings_->keys.end() ? it->second : c.keys;
        if (!keys.empty() && keys[0] == L'~') keys.erase(0, 1);
        return keys;
    }
    return {};
}

std::wstring MainWindow::MenuText(int id) const {
    for (const CommandDef& c : Commands()) {
        if (c.id != id) continue;
        std::wstring keys = KeysFor(id);
        return std::wstring(Tr(c.name)) + (keys.empty() ? L"" : L"\t" + keys);
    }
    return {};
}

void MainWindow::AppendCommand(HMENU menu, int id) {
    AppendMenuW(menu, MF_STRING, id, MenuText(id).c_str());
}

namespace {

// "Ctrl+Shift+F" -> ACCEL; false for text it does not understand.
bool ParseShortcut(std::wstring text, ACCEL& a) {
    a = {FVIRTKEY, 0, 0};
    text = Trim(text);
    if (text.empty()) return false;
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++) {  // split on '+', but "Ctrl++" style keys are written as "="
        if (i == text.size() || (text[i] == L'+' && i > start)) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        std::wstring m = Lower(parts[i]);
        if (m == L"ctrl") a.fVirt |= FCONTROL;
        else if (m == L"shift") a.fVirt |= FSHIFT;
        else if (m == L"alt") a.fVirt |= FALT;
        else return false;
    }
    std::wstring key = parts.back(), lk = Lower(key);
    static const std::pair<const wchar_t*, WORD> names[] = {
        {L"left", VK_LEFT}, {L"right", VK_RIGHT}, {L"up", VK_UP}, {L"down", VK_DOWN}, {L"space", VK_SPACE},
        {L"tab", VK_TAB}, {L"enter", VK_RETURN}, {L"esc", VK_ESCAPE}, {L"home", VK_HOME}, {L"end", VK_END},
        {L"pgup", VK_PRIOR}, {L"pgdn", VK_NEXT}, {L"ins", VK_INSERT}, {L"del", VK_DELETE}, {L"pause", VK_PAUSE},
        {L"backspace", VK_BACK}, {L"divide", VK_DIVIDE}, {L"add", VK_ADD}, {L"subtract", VK_SUBTRACT},
        {L"multiply", VK_MULTIPLY}, {L"/", VK_OEM_2}, {L"=", VK_OEM_PLUS}, {L"-", VK_OEM_MINUS}, {L".", VK_OEM_PERIOD},
        {L",", VK_OEM_COMMA}, {L";", VK_OEM_1}, {L"`", VK_OEM_3}, {L"[", VK_OEM_4}, {L"]", VK_OEM_6},
        {L"\\", VK_OEM_5}, {L"'", VK_OEM_7},
    };
    for (auto [name, vk] : names) {
        if (lk == name) {
            a.key = vk;
            return true;
        }
    }
    if (lk.size() >= 2 && lk[0] == L'f' && iswdigit(lk[1])) {
        int n = _wtoi(lk.c_str() + 1);
        if (n < 1 || n > 24) return false;
        a.key = static_cast<WORD>(VK_F1 + n - 1);
        return true;
    }
    if (key.size() == 1 && (iswalnum(key[0]))) {
        a.key = static_cast<WORD>(towupper(key[0]));
        return true;
    }
    return false;
}

std::vector<std::wstring> Alternatives(const std::wstring& keys) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t sep = keys.find(L" / ", start);
        out.push_back(Trim(keys.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start)));
        if (sep == std::wstring::npos) break;
        start = sep + 3;
    }
    return out;
}
}

void MainWindow::BuildAccelerators() {
    std::vector<ACCEL> accels;
    for (const CommandDef& c : Commands()) {
        auto it = settings_->keys.find(c.name);
        std::wstring keys = it != settings_->keys.end() ? it->second : c.keys;
        if (keys.empty() || keys[0] == L'~') continue;
        for (const std::wstring& k : Alternatives(keys)) {
            ACCEL a;
            if (!ParseShortcut(k, a)) continue;
            a.cmd = static_cast<WORD>(c.id);
            accels.push_back(a);
        }
    }
    if (accel_) DestroyAcceleratorTable(accel_);
    accel_ = CreateAcceleratorTableW(accels.data(), static_cast<int>(accels.size()));
}

// ---- command palette and quick open -------------------------------------------------------------------------------

void MainWindow::CommandPalette() {
    std::vector<int> ids;
    std::vector<std::wstring> items;
    for (const CommandDef& c : Commands()) {
        ids.push_back(c.id);
        items.push_back(std::wstring(Tr(c.name)) + L"\t" + KeysFor(c.id));
    }
    for (size_t i = 0; i < Theme::All().size(); i++) {
        ids.push_back(ID_THEME + static_cast<int>(i));
        items.push_back(std::wstring(Tr(L"Тема: ")) + Tr(Theme::All()[i].title) + L"\t");
    }
    auto pick = PickFromList(hwnd_, Tr(L"Палитра команд"), items, Tr(L"Название команды…"));
    if (pick) OnCommand(ids[*pick]);
}

std::vector<fs::path> MainWindow::ProjectFiles() {
    std::vector<fs::path> files;
    for (const fs::path& p : treePaths_) {
        if (!DirExists(p)) files.push_back(p);
    }
    return files;
}

void MainWindow::QuickOpen() {
    std::vector<fs::path> files = ProjectFiles();
    for (const auto& e : editors_) {  // open tabs from other folders too
        if (!e->file().empty() && std::none_of(files.begin(), files.end(), [&](const fs::path& f) { return SamePath(f, e->file()); })) {
            files.push_back(e->file());
        }
    }
    if (files.empty()) {
        ShowMessage(hwnd_, Tr(L"Откройте папку проекта (Ctrl+Shift+O)."), false);
        return;
    }
    std::vector<std::wstring> items;
    for (const fs::path& f : files) {
        fs::path rel = !projectRoot_.empty() && IsUnder(f, projectRoot_) ? NormalizePath(f).lexically_relative(NormalizePath(projectRoot_)) : f;
        items.push_back(f.filename().wstring() + L"\t" + rel.parent_path().wstring());
    }
    auto pick = PickFromList(hwnd_, Tr(L"Перейти к файлу"), items, Tr(L"Имя файла…"));
    if (pick) OpenFile(files[*pick]);
}

void MainWindow::EditKeys() {
    std::vector<std::pair<std::wstring, std::wstring>> commands;
    for (const CommandDef& c : Commands()) {
        if (c.keys[0] == L'~') continue;
        commands.push_back({c.name, c.keys});
    }
    if (!ShowKeysDialog(hwnd_, *settings_, commands)) return;
    settings_->Save();
    BuildAccelerators();
    Relocalize();  // menus show the new shortcuts
}

// ---- theme and language -------------------------------------------------------------------------------------------

void MainWindow::ApplyTheme(const std::wstring& id) {
    settings_->theme = id;
    Theme::Apply(id);
    InitDarkMode();
    ApplyDarkWindow(hwnd_);
    for (HWND h : {tree_, outline_}) {
        if (h) ApplyDarkControl(h);
    }
    for (auto& e : editors_) ApplyDarkControl(e->hwnd());
    for (auto& v : views_) ApplyDarkControl(v->hwnd());
    for (Editor* v : PanelViews()) ApplyDarkControl(v->hwnd());
    RestyleAll();
    for (auto& e : editors_) e->Call(SCI_COLOURISE, 0, -1);
    for (size_t i = 0; i < Theme::All().size(); i++) {
        CheckMenuItem(themeMenu_, ID_THEME + static_cast<UINT>(i),
                      id == Theme::All()[i].id ? MF_CHECKED : MF_UNCHECKED);
    }
    settings_->Save();
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

// Rebuilds everything that shows translated text (after the language or the shortcuts change).
void MainWindow::Relocalize() {
    for (HMENU m : {fileMenu_, editMenu_, viewMenu_, runMenu_, helpMenu_}) {
        if (m) DestroyMenu(m);  // destroys the submenus (recent, themes) too
    }
    BuildMenus();
    Layout();
    UpdateTitle();
    if (!debugging_) {
        RenderPanel(*regsView_, {{Tr(std::string("Панель заполняется, когда программа остановлена в отладчике.\n")), P_DIM}});
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- update check -------------------------------------------------------------------------------------------------

namespace {

std::string HttpGet(const wchar_t* host, const wchar_t* path) {
    std::string body;
    HINTERNET session = WinHttpOpen(L"AsmEditor", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD size = 0;
        while (WinHttpQueryDataAvailable(request, &size) && size > 0 && body.size() < 4'000'000) {
            std::string chunk(size, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), size, &read) || read == 0) break;
            body.append(chunk.data(), read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

std::vector<int> VersionParts(const std::string& v) {
    std::vector<int> parts;
    for (size_t i = 0; i < v.size();) {
        if (!isdigit(static_cast<unsigned char>(v[i]))) {
            i++;
            continue;
        }
        parts.push_back(atoi(v.c_str() + i));
        while (i < v.size() && isdigit(static_cast<unsigned char>(v[i]))) i++;
    }
    return parts;
}

// Highest version among the matches of `re` (group 1) in `text`.
std::string Newest(const std::string& text, const std::regex& re) {
    std::string best;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
        std::string v = (*it)[1].str();
        if (best.empty() || VersionParts(v) > VersionParts(best)) best = v;
    }
    return best;
}
}

void MainWindow::CheckUpdates() {
    Print("\n" + Tr(std::string("Проверка обновлений…")) + "\n", Out::Info);
    ShowPane(Pane::Output);
    std::wstring nasm = Toolchain::NasmVersion(*settings_);
    HWND hwnd = hwnd_;
    std::thread([hwnd, nasm] {
        auto post = [hwnd](const std::string& text, Out style) {
            PostMessageW(hwnd, WM_APP_INFO, static_cast<WPARAM>(style), reinterpret_cast<LPARAM>(new std::string(text)));
        };
        std::string nasmPage = HttpGet(L"www.nasm.us", L"/pub/nasm/releasebuilds/");
        std::string latestNasm = Newest(nasmPage, std::regex("href=\"(\\d+\\.\\d+(?:\\.\\d+)*)/\""));
        std::string sciPage = HttpGet(L"www.scintilla.org", L"/ScintillaDownload.html");
        std::string latestSci = Newest(sciPage, std::regex("[Ss]cintilla\\s*(?:version\\s*)?(\\d+\\.\\d+\\.\\d+)"));
        std::string current = "5.6.5";  // third_party/scintilla/version.txt
        if (latestNasm.empty() && latestSci.empty()) {
            post(Tr(std::string("Не удалось получить данные: нет подключения к интернету?")) + "\n", Out::Warning);
            return;
        }
        if (!latestNasm.empty()) {
            std::string have = Narrow(nasm);
            bool newer = !have.empty() && VersionParts(latestNasm) > VersionParts(have);
            post("NASM: " + Tr(std::string("установлен")) + " " + (have.empty() ? "—" : have) + ", " +
                     Tr(std::string("последний")) + " " + latestNasm +
                     (newer ? " — " + Tr(std::string("есть обновление: https://www.nasm.us")) : "") + "\n",
                 newer ? Out::Warning : Out::Success);
        }
        if (!latestSci.empty()) {
            bool newer = VersionParts(latestSci) > VersionParts(current);
            post("Scintilla: " + Tr(std::string("в редакторе")) + " " + current + ", " + Tr(std::string("последняя")) + " " +
                     latestSci + (newer ? " — " + Tr(std::string("есть обновление: https://www.scintilla.org")) : "") + "\n",
                 newer ? Out::Warning : Out::Success);
        }
    }).detach();
}

// Help → "Open .asm files in AsmEditor": per-user file association (or removing it again).
void MainWindow::ToggleAssociation() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (FileAssociation::IsRegistered(exe)) {
        std::wstring q = Tr(L"Файлы .asm уже открываются в AsmEditor. Убрать эту связь?");
        if (MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        FileAssociation::Unregister();
        Print(Tr(std::string("Связь файлов .asm с AsmEditor убрана.\n")), Out::Info);
        return;
    }
    std::wstring error;
    if (!FileAssociation::Register(exe, error)) {
        ShowMessage(hwnd_, std::wstring(Tr(L"Не удалось связать файлы: ")) + error);
        return;
    }
    ShowMessage(hwnd_, Tr(L"Готово: файлы .asm, .inc, .nasm и .mac открываются в AsmEditor (двойной клик в Проводнике). "
                          L"Если Windows спросит, чем открыть файл, выберите AsmEditor."), false);
}
