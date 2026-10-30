#include "mw_internal.h"
#include "project.h"

#include <psapi.h>

// ---- editors ----------------------------------------------------------------------------------------------------

Editor* MainWindow::FindByHwnd(HWND h) const {
    for (auto& e : editors_) if (e->hwnd() == h) return e.get();
    return nullptr;
}

Editor* MainWindow::FindById(int id) const {
    for (auto& e : editors_) if (e->id() == id) return e.get();
    return nullptr;
}

int MainWindow::FindByPath(const fs::path& p) const {
    for (size_t i = 0; i < editors_.size(); i++) if (SamePath(editors_[i]->file(), p)) return (int)i;
    return -1;
}

int MainWindow::IndexOf(const Editor* ed) const {
    for (size_t i = 0; i < editors_.size(); i++) if (editors_[i].get() == ed) return (int)i;
    return -1;
}

void MainWindow::AddEditor(std::unique_ptr<Editor> ed) {
    Editor* raw = ed.get();
    SetWindowSubclass(raw->hwnd(), EditorProc, 4, reinterpret_cast<DWORD_PTR>(this));
    editors_.push_back(std::move(ed));
    Activate((int)editors_.size() - 1);
    if (settings_->liveCheck) RequestCheck(raw);
}

// Shows an editor. With a split view the editor goes to the half that has the focus; choosing the editor that
// is already in the other half swaps the halves.
void MainWindow::Activate(int index) {
    if (editors_.empty()) {
        active_ = -1;
        splitIndex_ = -1;
        focusRight_ = false;
    } else {
        index = std::clamp(index, 0, (int)editors_.size() - 1);
        if (SplitEditor() && focusRight_) {
            if (index == active_) std::swap(active_, splitIndex_);
            else splitIndex_ = index;
        } else if (SplitEditor() && index == splitIndex_) {
            focusRight_ = true;
        } else {
            active_ = index;
        }
    }
    Layout();
    Editor* target = focusRight_ && SplitEditor() ? SplitEditor() : Active();
    if (target) SetFocus(target->hwnd());
    ensureTabVisible_ = true;
    UpdateTitle();
    if (leftTab_ == LeftTab::Outline) RefreshOutline();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::ConfirmClose(int index) {
    Editor& ed = *editors_[index];
    if (!ed.Modified()) return true;
    Activate(index);
    std::wstring q = std::wstring(Tr(L"Сохранить изменения в ")) + L"«" + ed.Title() + L"»?";
    int answer = MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return false;
    return answer == IDNO || Save(&ed);
}

void MainWindow::CloseEditor(int index) {
    if (index < 0 || index >= (int)editors_.size() || !ConfirmClose(index)) return;
    StashBreakpoints(editors_[index].get());
    RemoveBackup(editors_[index].get());
    Editor* left = Active();
    Editor* right = SplitEditor();
    Editor* closing = editors_[index].get();
    if (closing == usageEditor_) usageEditor_ = nullptr;
    if (closing == linkEditor_) linkEditor_ = nullptr;
    editors_.erase(editors_.begin() + index);
    if (closing == right) {
        splitIndex_ = -1;
        focusRight_ = false;
        right = nullptr;
    }
    if (closing == left) {  // the left half shows the neighbour (or what was on the right)
        int next = std::min(index, (int)editors_.size() - 1);
        if (right && next >= 0 && editors_[next].get() == right) next = next > 0 ? next - 1 : (editors_.size() > 1 ? 1 : -1);
        left = next >= 0 ? editors_[next].get() : nullptr;
        if (!left && right) {
            left = right;
            right = nullptr;
        }
    }
    active_ = IndexOf(left);
    splitIndex_ = right ? IndexOf(right) : -1;
    if (splitIndex_ < 0) focusRight_ = false;
    Activate(std::max(0, focusRight_ ? splitIndex_ : active_));
}

bool MainWindow::CloseAll() {
    for (int i = 0; i < (int)editors_.size(); i++) {
        if (!ConfirmClose(i)) return false;
    }
    for (auto& e : editors_) {
        StashBreakpoints(e.get());
        RemoveBackup(e.get());
    }
    usageEditor_ = linkEditor_ = nullptr;
    editors_.clear();
    splitIndex_ = -1;
    focusRight_ = false;
    Activate(-1);
    return true;
}

// Closes every tab except `keep` (-1: all of them); pinned tabs stay.
void MainWindow::CloseOthers(int keep) {
    Editor* kept = keep >= 0 && keep < (int)editors_.size() ? editors_[keep].get() : nullptr;
    for (int i = (int)editors_.size() - 1; i >= 0; i--) {
        if (i < (int)editors_.size() && editors_[i].get() != kept && !editors_[i]->pinned) CloseEditor(i);
    }
}

Editor* MainWindow::OpenFile(const fs::path& path, int line) {
    fs::path p = NormalizePath(path);
    int existing = FindByPath(p);
    if (existing >= 0) {
        Activate(existing);
    } else {
        auto ed = std::make_unique<Editor>(hwnd_, settings_->fontSize);
        std::wstring error;
        if (!ed->Load(p, error)) {
            ShowMessage(hwnd_, std::wstring(Tr(L"Не удалось открыть ")) + p.wstring() + L": " + error);
            return nullptr;
        }
        auto bps = breakpoints_.find(Debugger::Key(p));
        if (bps != breakpoints_.end()) {
            ed->SetBreakpoints(bps->second);
            breakpoints_.erase(bps);  // the editor's markers are the source of truth while it is open
        }
        AddEditor(std::move(ed));
    }
    Editor* ed = editors_[FindByPath(p) >= 0 ? FindByPath(p) : 0].get();
    if (ed && line > 0) ed->GotoLine(line);
    return ed;
}

bool MainWindow::Save(Editor* ed) {
    if (!ed) return false;
    if (ed->file().empty()) return SaveAs(ed);
    std::wstring error;
    if (!ed->Save(ed->file(), error)) {
        ShowMessage(hwnd_, std::wstring(Tr(L"Не удалось сохранить: ")) + error);
        return false;
    }
    RemoveBackup(ed);
    InvalidateChrome();
    return true;
}

bool MainWindow::SaveAs(Editor* ed) {
    if (!ed) return false;
    wchar_t buf[MAX_PATH * 4];
    wcsncpy_s(buf, ed->file().empty() ? ed->Title().c_str() : ed->file().c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Ассемблер (*.asm)\0*.asm\0Все файлы\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buf));
    ofn.lpstrDefExt = L"asm";
    std::wstring initial = projectRoot_.wstring();
    ofn.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    RemoveBackup(ed);
    std::wstring error;
    if (!ed->Save(buf, error)) {
        ShowMessage(hwnd_, std::wstring(Tr(L"Не удалось сохранить: ")) + error);
        return false;
    }
    RefreshTree();
    UpdateTitle();
    InvalidateChrome();
    if (settings_->liveCheck) RequestCheck(ed);
    return true;
}

void MainWindow::SaveAll() {
    for (auto& e : editors_) {
        if (e->Modified()) Save(e.get());
    }
}

void MainWindow::ApplyFontSize(int size) {
    settings_->fontSize = std::clamp(size, 6, 40);
    RestyleAll();
    settings_->Save();
}

// Re-applies font size and theme colors to every view (after a font size or theme change).
void MainWindow::RestyleAll() {
    for (auto& e : editors_) e->ApplyStyles(settings_->fontSize);
    for (auto& view : views_) {
        view->ApplyStyles(settings_->fontSize);
        SetupOutputView(*view);
    }
    for (Editor* v : PanelViews()) {
        v->ApplyStyles(settings_->fontSize);
        SetupPanelView(*v);
    }
    for (HWND t : {tree_, outline_}) {
        TreeView_SetBkColor(t, Theme::PanelBg);
        TreeView_SetTextColor(t, Theme::Text);
    }
    if (inputBrush_) DeleteObject(inputBrush_);
    inputBrush_ = CreateSolidBrush(Theme::InputBg);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- tabs ---------------------------------------------------------------------------------------------------------

void MainWindow::TabMenu(int index) {
    if (index < 0 || index >= (int)editors_.size()) return;
    Editor* ed = editors_[index].get();
    HMENU menu = CreatePopupMenu();
    enum { kClose = 1, kOthers, kAll, kPin, kSplit, kUnsplit, kReveal, kMain, kCopyPath };
    AppendMenuW(menu, MF_STRING, kClose, Tr(L"Закрыть"));
    AppendMenuW(menu, MF_STRING, kOthers, Tr(L"Закрыть другие"));
    AppendMenuW(menu, MF_STRING, kAll, Tr(L"Закрыть все"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kPin, ed->pinned ? Tr(L"Открепить") : Tr(L"Закрепить"));
    if (index != splitIndex_) AppendMenuW(menu, MF_STRING, kSplit, Tr(L"Открыть справа"));
    if (SplitEditor()) AppendMenuW(menu, MF_STRING, kUnsplit, Tr(L"Убрать разделение"));
    if (!ed->file().empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (Project::IsSourceFile(ed->file())) {
            AppendMenuW(menu, MF_STRING, kMain, SamePath(ed->file(), mainFile_) ? Tr(L"Убрать отметку главного файла")
                                                                               : Tr(L"Сделать главным файлом (запуск)"));
        }
        AppendMenuW(menu, MF_STRING, kCopyPath, Tr(L"Копировать путь"));
        AppendMenuW(menu, MF_STRING, kReveal, Tr(L"Показать в проводнике"));
    }
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case kClose: CloseEditor(index); break;
    case kOthers: CloseOthers(index); break;
    case kAll: CloseOthers(-1); break;
    case kPin: TogglePin(index); break;
    case kSplit: SplitWith(index); break;
    case kUnsplit: Unsplit(); break;
    case kMain: SetMainFile(SamePath(ed->file(), mainFile_) ? fs::path() : ed->file()); break;
    case kCopyPath:
        if (OpenClipboard(hwnd_)) {
            std::wstring text = ed->file().wstring();
            EmptyClipboard();
            if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
                memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(mem);
                SetClipboardData(CF_UNICODETEXT, mem);
            }
            CloseClipboard();
        }
        break;
    case kReveal: {
        std::wstring args = L"/select,\"" + ed->file().wstring() + L"\"";
        ShellExecuteW(hwnd_, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    }
}

void MainWindow::TogglePin(int index) {
    if (index < 0 || index >= (int)editors_.size()) return;
    editors_[index]->pinned = !editors_[index]->pinned;
    SortPinnedTabs();
    SaveProjectSession();
    InvalidateRect(hwnd_, &rcTabs_, FALSE);
}

// Moves a tab, keeping which editors are shown (indices are recomputed from the editors themselves).
void MainWindow::MoveTab(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= (int)editors_.size() || to >= (int)editors_.size()) return;
    Editor* left = Active();
    Editor* right = SplitEditor();
    auto moving = std::move(editors_[from]);
    editors_.erase(editors_.begin() + from);
    editors_.insert(editors_.begin() + to, std::move(moving));
    active_ = IndexOf(left);
    splitIndex_ = right ? IndexOf(right) : -1;
}

void MainWindow::SortPinnedTabs() {
    Editor* left = Active();
    Editor* right = SplitEditor();
    std::stable_partition(editors_.begin(), editors_.end(), [](const auto& e) { return e->pinned; });
    active_ = IndexOf(left);
    splitIndex_ = right ? IndexOf(right) : -1;
}

// Shows the editor in the right half; the left half keeps (or gets) another one.
void MainWindow::SplitWith(int index) {
    if (index < 0 || index >= (int)editors_.size()) return;
    if (editors_.size() < 2) {
        ShowMessage(hwnd_, Tr(L"Для разделения нужны хотя бы две вкладки: откройте второй файл."), false);
        return;
    }
    if (index == active_) {  // the left half shows the previous tab instead
        active_ = index > 0 ? index - 1 : index + 1;
    }
    splitIndex_ = index;
    focusRight_ = true;
    Activate(index);
}

void MainWindow::Unsplit() {
    if (!SplitEditor()) return;
    int keep = focusRight_ ? splitIndex_ : active_;
    splitIndex_ = -1;
    focusRight_ = false;
    Activate(keep);
}

// ---- project ----------------------------------------------------------------------------------------------------

std::optional<fs::path> MainWindow::PickFolder(const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    std::optional<fs::path> result;
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

void MainWindow::OpenPath(const fs::path& path) {
    if (DirExists(path)) OpenProject(path);
    else if (FileExists(path.wstring())) OpenFile(path);
    else ShowMessage(hwnd_, std::wstring(Tr(L"Не найдено: ")) + path.wstring());
}

bool MainWindow::OpenProject(const fs::path& dirIn) {
    fs::path dir = NormalizePath(dirIn);
    if (SamePath(dir, projectRoot_)) return true;
    SaveProjectSession();
    if (!CloseAll()) return false;
    projectRoot_ = dir;
    mainFile_.clear();
    AddRecent(dir);
    settings_->lastProject = dir.wstring();
    settings_->Save();
    Log::Write("open project " + Narrow(dir.wstring()));

    auto kv = ReadKeyValues(dir / L".asmeditor" / L"project.ini");
    if (!kv[L"main"].empty() && FileExists((dir / kv[L"main"]).wstring())) mainFile_ = NormalizePath(dir / kv[L"main"]);
    excludedFromBuild_.clear();
    for (const std::wstring& rel : Split(kv[L"exclude"], L'|')) excludedFromBuild_.insert(Project::Key(dir / rel));
    breakpoints_.clear();
    for (const std::wstring& entry : Split(kv[L"breakpoints"], L'|')) {  // "relative*3,7,12"
        size_t star = entry.find(L'*');
        if (star == std::wstring::npos) continue;
        auto& lines = breakpoints_[Debugger::Key(dir / entry.substr(0, star))];
        for (const std::wstring& n : Split(entry.substr(star + 1), L',')) lines[_wtoi(n.c_str())] = "";
    }
    for (const std::wstring& entry : Split(kv[L"conditions"], L'\t')) {  // "relative*line*condition"
        size_t a = entry.find(L'*'), b = a == std::wstring::npos ? a : entry.find(L'*', a + 1);
        if (b == std::wstring::npos) continue;
        breakpoints_[Debugger::Key(dir / entry.substr(0, a))][_wtoi(entry.substr(a + 1, b - a - 1).c_str())] =
            Narrow(UnescapeValue(entry.substr(b + 1)));
    }
    runConfig_ = RunConfig{};
    if (!kv[L"run.args"].empty()) runConfig_.args = UnescapeValue(kv[L"run.args"]);
    runConfig_.workDir = UnescapeValue(kv[L"run.dir"]);
    runConfig_.env = UnescapeValue(kv[L"run.env"]);
    runConfig_.stdinFile = UnescapeValue(kv[L"run.stdin"]);
    runConfig_.preBuild = UnescapeValue(kv[L"run.pre"]);
    runConfig_.postBuild = UnescapeValue(kv[L"run.post"]);
    if (!kv[L"run.output"].empty()) runConfig_.output = kv[L"run.output"];
    if (!kv[L"run.profile"].empty()) runConfig_.profile = kv[L"run.profile"];
    if (!kv[L"run.platform"].empty()) runConfig_.platform = kv[L"run.platform"];
    watches_.clear();
    for (const std::wstring& w : Split(kv[L"watches"], L'\t')) watches_.push_back(Narrow(UnescapeValue(w)));
    std::set<std::wstring> pinned;
    for (const std::wstring& f : Split(kv[L"pinned"], L'|')) pinned.insert(Project::Key(dir / f));
    for (const std::wstring& f : Split(kv[L"open"], L'|')) {
        fs::path p = dir / f;
        if (!FileExists(p.wstring())) continue;
        if (Editor* ed = OpenFile(p)) ed->pinned = pinned.count(Project::Key(p)) > 0;
    }
    SortPinnedTabs();
    int active = kv[L"active"].empty() ? -1 : _wtoi(kv[L"active"].c_str());
    if (active >= 0 && active < (int)editors_.size()) Activate(active);
    RefreshTree();
    UpdateTitle();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void MainWindow::CloseProject() {
    if (projectRoot_.empty()) return;
    SaveProjectSession();
    if (!CloseAll()) return;
    projectRoot_.clear();
    mainFile_.clear();
    runConfig_ = RunConfig{};
    watches_.clear();
    settings_->lastProject.clear();
    settings_->Save();
    RefreshTree();
    UpdateTitle();
    Layout();
}

void MainWindow::SaveProjectSession() {
    if (projectRoot_.empty()) {
        settings_->sessionFiles.clear();
        for (auto& e : editors_) {
            if (!e->file().empty()) settings_->sessionFiles.push_back(e->file().wstring());
        }
        return;
    }
    std::wstring rootKey = Lower(NormalizePath(projectRoot_).wstring());
    std::vector<std::wstring> open, pinned;
    for (auto& e : editors_) {
        if (!IsUnder(e->file(), projectRoot_)) continue;
        std::wstring rel = e->file().lexically_relative(projectRoot_).wstring();
        open.push_back(rel);
        if (e->pinned) pinned.push_back(rel);
    }
    std::vector<std::wstring> bps, conditions;
    for (const auto& [key, lines] : CollectBreakpoints()) {
        if (!IsUnder(key, projectRoot_) || lines.empty()) continue;
        std::wstring rel = fs::path(key).lexically_relative(rootKey).wstring();
        std::vector<std::wstring> nums;
        for (const auto& [line, condition] : lines) {
            nums.push_back(std::to_wstring(line));
            if (!condition.empty()) conditions.push_back(rel + L"*" + std::to_wstring(line) + L"*" + EscapeValue(Widen(condition)));
        }
        bps.push_back(rel + L"*" + Join(nums, L","));
    }
    std::vector<std::wstring> excluded;
    for (const std::wstring& key : excludedFromBuild_) {
        if (IsUnder(key, projectRoot_)) excluded.push_back(fs::path(key).lexically_relative(rootKey).wstring());
    }
    std::vector<std::wstring> watches;
    for (const std::string& w : watches_) watches.push_back(EscapeValue(Widen(w)));
    std::wstring text = L"main=" + (IsUnder(mainFile_, projectRoot_) ? mainFile_.lexically_relative(projectRoot_).wstring() : L"") +
                        L"\r\nopen=" + Join(open, L"|") + L"\r\npinned=" + Join(pinned, L"|") +
                        L"\r\nactive=" + std::to_wstring(active_) +
                        L"\r\nbreakpoints=" + Join(bps, L"|") + L"\r\nconditions=" + Join(conditions, L"\t") +
                        L"\r\nexclude=" + Join(excluded, L"|") + L"\r\nwatches=" + Join(watches, L"\t") +
                        L"\r\nrun.args=" + EscapeValue(runConfig_.args) + L"\r\nrun.dir=" + EscapeValue(runConfig_.workDir) +
                        L"\r\nrun.env=" + EscapeValue(runConfig_.env) + L"\r\nrun.stdin=" + EscapeValue(runConfig_.stdinFile) +
                        L"\r\nrun.pre=" + EscapeValue(runConfig_.preBuild) + L"\r\nrun.post=" + EscapeValue(runConfig_.postBuild) +
                        L"\r\nrun.output=" + runConfig_.output + L"\r\nrun.profile=" + runConfig_.profile +
                        L"\r\nrun.platform=" + runConfig_.platform + L"\r\n";
    fs::path dir = projectRoot_ / L".asmeditor";
    std::error_code ec;
    fs::create_directories(dir, ec);
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN);
    std::ofstream out(dir / L"project.ini", std::ios::binary | std::ios::trunc);
    std::string bytes = Narrow(text);
    out.write(bytes.data(), (std::streamsize)bytes.size());
}

void MainWindow::RestoreSession() {
    if (!settings_->lastProject.empty() && DirExists(settings_->lastProject)) {
        OpenProject(settings_->lastProject);
    } else {
        for (const std::wstring& f : settings_->sessionFiles) {
            if (FileExists(f)) OpenFile(f);
        }
    }
    OfferRecovery();
}

void MainWindow::NewProject() {
    std::vector<std::wstring> items;
    for (const Templates::Template& t : Templates::All()) items.push_back(Tr(t.title));
    auto pick = PickFromList(hwnd_, Tr(L"Новый проект: шаблон"), items, Tr(L"Название шаблона…"));
    if (!pick) return;
    const Templates::Template& t = Templates::All()[*pick];
    auto dir = PickFolder(Tr(L"Папка для нового проекта (можно создать новую)"));
    if (!dir) return;
    fs::path main = *dir / t.fileName;
    if (fs::exists(main)) {
        std::wstring q = std::wstring(Tr(L"Файл уже существует: ")) + main.wstring() + L"\n" + Tr(L"Оставить его как есть?");
        if (MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            std::ofstream out(main, std::ios::binary | std::ios::trunc);
            out << t.text;
        }
    } else {
        std::ofstream out(main, std::ios::binary);
        out << t.text;
    }
    if (!OpenProject(*dir)) return;
    SetMainFile(main);
    OpenFile(main);
    RefreshTree();
}

void MainWindow::SetMainFile(const fs::path& f) {
    mainFile_ = f.empty() ? fs::path() : NormalizePath(f);
    SaveProjectSession();
    RefreshTree();
    Layout();
    InvalidateChrome();
}

void MainWindow::AddRecent(const fs::path& dir) {
    auto& r = settings_->recent;
    r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& s) { return SamePath(s, dir); }), r.end());
    r.insert(r.begin(), dir.wstring());
    if (r.size() > 10) r.resize(10);
}

fs::path MainWindow::TreePath(HTREEITEM item) const {
    if (!item) return {};
    TVITEMW tvi{};
    tvi.mask = TVIF_PARAM;
    tvi.hItem = item;
    TreeView_GetItem(tree_, &tvi);
    size_t index = static_cast<size_t>(tvi.lParam);
    return index < treePaths_.size() ? treePaths_[index] : fs::path();
}

void MainWindow::BuildTree(HTREEITEM parent, const fs::path& dir, int depth) {
    std::vector<fs::path> dirs, files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (!kHiddenDirs.count(it->path().filename().wstring())) dirs.push_back(it->path());
        } else {
            files.push_back(it->path());
        }
    }
    auto byName = [](const fs::path& a, const fs::path& b) {
        return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    auto insert = [&](const fs::path& p, bool folder) {
        SHFILEINFOW sfi{};
        SHGetFileInfoW(p.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        std::wstring name = p.filename().wstring();
        bool isMain = SamePath(p, mainFile_);
        if (isMain) name += L"   ▶ main";
        else if (!folder && excludedFromBuild_.count(Project::Key(p))) name += std::wstring(L"   — ") + Tr(L"не собирается");
        TVINSERTSTRUCTW ins{};
        ins.hParent = parent;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_STATE | TVIF_CHILDREN;
        ins.item.pszText = name.data();
        ins.item.iImage = ins.item.iSelectedImage = sfi.iIcon;
        ins.item.lParam = static_cast<LPARAM>(treePaths_.size());
        ins.item.state = isMain ? TVIS_BOLD : 0;
        ins.item.stateMask = TVIS_BOLD;
        ins.item.cChildren = folder ? 1 : 0;
        treePaths_.push_back(p);
        HTREEITEM item = TreeView_InsertItem(tree_, &ins);
        if (folder && depth < 16) BuildTree(item, p, depth + 1);
    };
    for (const fs::path& d : dirs) insert(d, true);
    for (const fs::path& f : files) insert(f, false);
}

void MainWindow::RefreshTree() {
    // Remember expanded folders and the selection.
    std::set<std::wstring> expanded;
    std::function<void(HTREEITEM)> collect = [&](HTREEITEM item) {
        for (; item; item = TreeView_GetNextSibling(tree_, item)) {
            if (TreeView_GetItemState(tree_, item, TVIS_EXPANDED) & TVIS_EXPANDED) {
                expanded.insert(Lower(TreePath(item).wstring()));
                collect(TreeView_GetChild(tree_, item));
            }
        }
    };
    collect(TreeView_GetRoot(tree_));
    std::wstring selected = Lower(TreePath(TreeView_GetSelection(tree_)).wstring());

    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    treePaths_.clear();
    if (!projectRoot_.empty()) {
        SHFILEINFOW sfi{};
        SHGetFileInfoW(projectRoot_.c_str(), FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        std::wstring name = projectRoot_.filename().wstring();
        if (name.empty()) name = projectRoot_.wstring();
        TVINSERTSTRUCTW ins{};
        ins.hParent = TVI_ROOT;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_STATE;
        ins.item.pszText = name.data();
        ins.item.iImage = ins.item.iSelectedImage = sfi.iIcon;
        ins.item.lParam = 0;
        ins.item.state = TVIS_BOLD;
        ins.item.stateMask = TVIS_BOLD;
        treePaths_.push_back(projectRoot_);
        HTREEITEM root = TreeView_InsertItem(tree_, &ins);
        BuildTree(root, projectRoot_, 0);
        expanded.insert(Lower(projectRoot_.wstring()));

        std::function<void(HTREEITEM)> restore = [&](HTREEITEM item) {
            for (; item; item = TreeView_GetNextSibling(tree_, item)) {
                std::wstring key = Lower(TreePath(item).wstring());
                if (expanded.count(key)) {
                    TreeView_Expand(tree_, item, TVE_EXPAND);
                    restore(TreeView_GetChild(tree_, item));
                }
                if (key == selected) TreeView_SelectItem(tree_, item);
            }
        };
        restore(root);
    }
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, TRUE);
    InvalidateRect(hwnd_, &rcTreeHeader_, FALSE);
}

void MainWindow::TreeContextMenu() {
    if (projectRoot_.empty()) return;
    fs::path target = contextItem_ ? TreePath(contextItem_) : projectRoot_;
    bool file = !target.empty() && !DirExists(target);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TREE_NEW_FILE, Tr(L"Новый файл…"));
    AppendMenuW(menu, MF_STRING, ID_TREE_NEW_FOLDER, Tr(L"Новая папка…"));
    if (file) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TREE_OPEN, Tr(L"Открыть"));
        AppendMenuW(menu, MF_STRING, ID_TREE_MAIN,
                    SamePath(target, mainFile_) ? Tr(L"Убрать отметку главного файла") : Tr(L"Сделать главным файлом (запуск)"));
        if (Project::IsSourceFile(target)) {
            AppendMenuW(menu, MF_STRING, ID_TREE_EXCLUDE, excludedFromBuild_.count(Project::Key(target))
                                                               ? Tr(L"Собирать этот файл")
                                                               : Tr(L"Не собирать этот файл"));
        }
    }
    if (!SamePath(target, projectRoot_)) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TREE_RENAME, Tr(L"Переименовать…"));
        AppendMenuW(menu, MF_STRING, ID_TREE_DELETE, Tr(L"Удалить в корзину"));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TREE_REVEAL, Tr(L"Показать в проводнике"));
    AppendMenuW(menu, MF_STRING, ID_REFRESH_TREE, Tr(L"Обновить"));
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
}

std::vector<fs::path> MainWindow::ProgramFiles() {
    std::vector<fs::path> out;
    for (const fs::path& p : ProjectFiles()) {
        if (Project::IsSourceFile(p) && Project::DefinesMain(p)) out.push_back(p);
    }
    return out;
}

// ---- outline ------------------------------------------------------------------------------------------------------

void MainWindow::ShowLeftTab(LeftTab tab) {
    leftTab_ = tab;
    if (tab == LeftTab::Outline) RefreshOutline();
    Layout();
}

// Sections, labels (local ones under their global label), macros / constants and external names of the file
// in the focused editor. Clicking an item shows its line.
void MainWindow::RefreshOutline() {
    Editor* ed = focusRight_ && SplitEditor() ? SplitEditor() : Active();
    SendMessageW(outline_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(outline_);
    outlineLines_.clear();
    outlineEditor_ = ed ? ed->id() : 0;
    auto add = [&](HTREEITEM parent, const std::wstring& text, int line, bool bold) {
        TVINSERTSTRUCTW ins{};
        ins.hParent = parent;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
        std::wstring t = text;
        ins.item.pszText = t.data();
        ins.item.lParam = static_cast<LPARAM>(outlineLines_.size());
        ins.item.state = bold ? TVIS_BOLD : 0;
        ins.item.stateMask = TVIS_BOLD;
        outlineLines_.push_back(line);
        return TreeView_InsertItem(outline_, &ins);
    };
    if (ed) {
        std::string text = ed->Text();
        // Sections are not symbols: find "section .xxx" lines.
        std::vector<std::pair<int, std::string>> sections;
        {
            std::vector<Token> tokens;
            size_t pos = 0;
            int lineNo = 0;
            while (pos <= text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                lineNo++;
                Tokenize(text.data() + pos, static_cast<int>(end - pos), tokens);
                if (!tokens.empty() && tokens[0].type == T_DIRECTIVE) {
                    std::string w = text.substr(pos + tokens[0].start, tokens[0].end - tokens[0].start);
                    for (char& c : w) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    if (w == "section" || w == "segment") {
                        std::string rest = text.substr(pos + tokens[0].end, end - pos - tokens[0].end);
                        size_t s = rest.find_first_not_of(" \t"), e = rest.find_first_of(" \t;\r", s);
                        sections.push_back({lineNo, s == std::string::npos ? std::string() : rest.substr(s, e - s)});
                    }
                }
                pos = end + 1;
            }
        }
        std::vector<Completion::Symbol> symbols = Completion::ScanSymbols(text);
        HTREEITEM macros = nullptr, externs = nullptr;
        size_t nextSection = 0;
        HTREEITEM section = nullptr, global = nullptr;
        for (const auto& s : symbols) {
            while (nextSection < sections.size() && sections[nextSection].first <= s.line) {
                section = add(TVI_ROOT, L"section " + Widen(sections[nextSection].second), sections[nextSection].first, true);
                global = nullptr;
                nextSection++;
            }
            std::wstring label = Widen(s.name) + L"   :" + std::to_wstring(s.line);
            if (s.declaration) {
                if (s.kind != Completion::Kind::Function) continue;  // global: the label itself is listed
                if (!externs) externs = add(TVI_ROOT, Tr(L"Внешние (extern)"), s.line, true);
                add(externs, label, s.line, false);
            } else if (s.kind == Completion::Kind::Macro) {
                if (!macros) macros = add(TVI_ROOT, Tr(L"Макросы и константы"), s.line, true);
                add(macros, label, s.line, false);
            } else if (s.name[0] == '.' && s.name.rfind("..", 0) != 0 && global) {
                add(global, label, s.line, false);
            } else {
                global = add(section ? section : TVI_ROOT, label, s.line, false);
            }
        }
        while (nextSection < sections.size()) {
            add(TVI_ROOT, L"section " + Widen(sections[nextSection].second), sections[nextSection].first, true);
            nextSection++;
        }
        std::function<void(HTREEITEM)> expand = [&](HTREEITEM item) {
            for (; item; item = TreeView_GetNextSibling(outline_, item)) {
                TreeView_Expand(outline_, item, TVE_EXPAND);
                expand(TreeView_GetChild(outline_, item));
            }
        };
        expand(TreeView_GetRoot(outline_));
        if (outlineLines_.empty()) add(TVI_ROOT, Tr(L"(нет меток)"), 1, false);
    }
    SendMessageW(outline_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(outline_, nullptr, TRUE);
}

// ---- files changed on disk, autosave, crash recovery -------------------------------------------------------------

void MainWindow::CheckExternalChanges() {
    static bool busy = false;  // the question box re-activates the window
    if (busy) return;
    busy = true;
    for (size_t i = 0; i < editors_.size(); i++) {
        Editor* ed = editors_[i].get();
        if (!ed->ChangedOnDisk()) continue;
        if (!FileExists(ed->file().wstring())) {
            ed->ForgetDiskChange();
            continue;
        }
        bool reload = !ed->Modified();
        if (!reload) {
            std::wstring q = std::wstring(Tr(L"Файл изменён другой программой: ")) + ed->file().wstring() + L"\n\n" +
                             Tr(L"Перечитать его с диска? Несохранённые изменения в редакторе будут потеряны.");
            reload = MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNO | MB_ICONWARNING) == IDYES;
        }
        if (!reload) {
            ed->ForgetDiskChange();
            continue;
        }
        std::wstring error;
        if (ed->Reload(error)) {
            Print(Tr(std::string("Файл изменён на диске и перечитан: ")) + Narrow(ed->file().filename().wstring()) + "\n", Out::Info);
            if (settings_->liveCheck) RequestCheck(ed);
        }
    }
    busy = false;
    InvalidateChrome();
}

fs::path MainWindow::BackupDir() const {
    return Settings::Dir() / L"backup" / std::to_wstring(GetCurrentProcessId());
}

// Every 30 seconds: the text of each modified tab goes to %APPDATA%\AsmEditor\backup\<pid>\, so it can be
// restored if the editor crashes. Saving or closing a tab removes its copy.
void MainWindow::WriteBackups() {
    fs::path dir = BackupDir();
    std::error_code ec;
    for (auto& e : editors_) {
        fs::path file = dir / (L"tab" + std::to_wstring(e->id()) + L".txt");
        if (!e->Modified()) {
            fs::remove(file, ec);
            continue;
        }
        fs::create_directories(dir, ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        std::string head = "path=" + Narrow(e->file().wstring()) + "\ntitle=" + Narrow(e->Title()) + "\n\n";
        std::string text = e->Text();
        out.write(head.data(), static_cast<std::streamsize>(head.size()));
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

void MainWindow::RemoveBackup(const Editor* ed) {
    std::error_code ec;
    fs::remove(BackupDir() / (L"tab" + std::to_wstring(ed->id()) + L".txt"), ec);
    if (fs::is_empty(BackupDir(), ec)) fs::remove(BackupDir(), ec);
}

namespace {
// The backup folder belongs to a running AsmEditor (not a crashed one).
bool OwnerAlive(DWORD pid) {
    if (pid == GetCurrentProcessId()) return true;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    wchar_t name[MAX_PATH] = L"";
    DWORD size = MAX_PATH;
    bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE && QueryFullProcessImageNameW(p, 0, name, &size) &&
                 _wcsicmp(fs::path(name).filename().c_str(), L"AsmEditor.exe") == 0;
    CloseHandle(p);
    return alive;
}
}

// After a crash: offers to bring back the unsaved texts the previous run left in the backup folder.
void MainWindow::OfferRecovery() {
    fs::path root = Settings::Dir() / L"backup";
    std::error_code ec;
    struct Found {
        fs::path backup, file;
        std::wstring title;
        std::string text;
    };
    std::vector<Found> found;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory() || OwnerAlive(static_cast<DWORD>(_wtoi(it->path().filename().c_str())))) continue;
        dirs.push_back(it->path());
        std::error_code e2;
        for (fs::directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
            std::ifstream in(f->path(), std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string data = ss.str();
            size_t split = data.find("\n\n");
            if (split == std::string::npos) continue;
            Found item{f->path()};
            for (const std::wstring& line : Split(Widen(data.substr(0, split)), L'\n')) {
                if (line.rfind(L"path=", 0) == 0) item.file = line.substr(5);
                if (line.rfind(L"title=", 0) == 0) item.title = line.substr(6);
            }
            item.text = data.substr(split + 2);
            found.push_back(std::move(item));
        }
    }
    if (dirs.empty()) return;
    if (!found.empty()) {
        std::wstring list;
        for (const Found& f : found) list += L"\n• " + (f.file.empty() ? f.title : f.file.wstring());
        std::wstring q = std::wstring(Tr(L"Редактор был закрыт аварийно. Найдены несохранённые изменения:")) + list + L"\n\n" +
                         Tr(L"Восстановить их во вкладках (файлы на диске не изменятся, пока вы не сохраните)?");
        if (MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            for (const Found& f : found) {
                Editor* ed = nullptr;
                if (!f.file.empty() && FileExists(f.file.wstring())) {
                    ed = OpenFile(f.file);
                } else {
                    AddEditor(std::make_unique<Editor>(hwnd_, settings_->fontSize));
                    ed = Active();
                }
                if (!ed) continue;
                ed->Call(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(f.text.c_str()));  // stays modified, undoable
            }
        }
    }
    for (const fs::path& d : dirs) fs::remove_all(d, ec);
}
