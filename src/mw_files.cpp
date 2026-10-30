#include "mw_internal.h"
#include "project.h"

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

void MainWindow::AddEditor(std::unique_ptr<Editor> ed) {
    Editor* raw = ed.get();
    SetWindowSubclass(raw->hwnd(), EditorProc, 4, reinterpret_cast<DWORD_PTR>(this));
    editors_.push_back(std::move(ed));
    Layout();
    Activate((int)editors_.size() - 1);
    if (settings_->liveCheck) RequestCheck(raw);
}

void MainWindow::Activate(int index) {
    if (editors_.empty()) {
        active_ = -1;
    } else {
        active_ = std::clamp(index, 0, (int)editors_.size() - 1);
    }
    for (size_t i = 0; i < editors_.size(); i++) ShowWindow(editors_[i]->hwnd(), (int)i == active_ ? SW_SHOW : SW_HIDE);
    if (Editor* ed = Active()) SetFocus(ed->hwnd());
    ensureTabVisible_ = true;
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::ConfirmClose(int index) {
    Editor& ed = *editors_[index];
    if (!ed.Modified()) return true;
    Activate(index);
    std::wstring q = L"Сохранить изменения в «" + ed.Title() + L"»?";
    int answer = MessageBoxW(hwnd_, q.c_str(), L"AsmEditor", MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return false;
    return answer == IDNO || Save(&ed);
}

void MainWindow::CloseEditor(int index) {
    if (index < 0 || index >= (int)editors_.size() || !ConfirmClose(index)) return;
    StashBreakpoints(editors_[index].get());
    editors_.erase(editors_.begin() + index);
    if (active_ >= index) active_--;
    Activate(std::max(0, active_));
}

bool MainWindow::CloseAll() {
    for (int i = 0; i < (int)editors_.size(); i++) {
        if (!ConfirmClose(i)) return false;
    }
    for (auto& e : editors_) StashBreakpoints(e.get());
    editors_.clear();
    Activate(-1);
    return true;
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
            ShowMessage(hwnd_, L"Не удалось открыть " + p.wstring() + L": " + error);
            return nullptr;
        }
        auto bps = breakpoints_.find(Debugger::Key(p));
        if (bps != breakpoints_.end()) {
            ed->SetBreakpoints(bps->second);
            breakpoints_.erase(bps);  // the editor's markers are the source of truth while it is open
        }
        AddEditor(std::move(ed));
    }
    Editor* ed = Active();
    if (ed && line > 0) ed->GotoLine(line);
    return ed;
}

bool MainWindow::Save(Editor* ed) {
    if (!ed) return false;
    if (ed->file().empty()) return SaveAs(ed);
    std::wstring error;
    if (!ed->Save(ed->file(), error)) {
        ShowMessage(hwnd_, L"Не удалось сохранить: " + error);
        return false;
    }
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
    std::wstring error;
    if (!ed->Save(buf, error)) {
        ShowMessage(hwnd_, L"Не удалось сохранить: " + error);
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
    for (auto& e : editors_) e->ApplyStyles(settings_->fontSize);
    console_->Call(SCI_STYLESETSIZE, STYLE_DEFAULT, settings_->fontSize);
    for (int s = 0; s <= static_cast<int>(Out::Input); s++) console_->Call(SCI_STYLESETSIZE, s, settings_->fontSize);
    settings_->Save();
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
    else ShowMessage(hwnd_, L"Не найдено: " + path.wstring());
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

    auto kv = ReadKeyValues(dir / L".asmeditor" / L"project.ini");
    if (!kv[L"main"].empty() && FileExists((dir / kv[L"main"]).wstring())) mainFile_ = NormalizePath(dir / kv[L"main"]);
    excludedFromBuild_.clear();
    for (const std::wstring& rel : Split(kv[L"exclude"], L'|')) excludedFromBuild_.insert(Project::Key(dir / rel));
    for (const std::wstring& entry : Split(kv[L"breakpoints"], L'|')) {  // "relative*3,7,12"
        size_t star = entry.find(L'*');
        if (star == std::wstring::npos) continue;
        auto& lines = breakpoints_[Debugger::Key(dir / entry.substr(0, star))];
        for (const std::wstring& n : Split(entry.substr(star + 1), L',')) lines.insert(_wtoi(n.c_str()));
    }
    for (const std::wstring& f : Split(kv[L"open"], L'|')) {
        fs::path p = dir / f;
        if (FileExists(p.wstring())) OpenFile(p);
    }
    int active = kv[L"active"].empty() ? -1 : _wtoi(kv[L"active"].c_str());
    if (active >= 0 && active < (int)editors_.size()) Activate(active);
    RefreshTree();
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void MainWindow::CloseProject() {
    if (projectRoot_.empty()) return;
    SaveProjectSession();
    if (!CloseAll()) return;
    projectRoot_.clear();
    mainFile_.clear();
    settings_->lastProject.clear();
    settings_->Save();
    RefreshTree();
    UpdateTitle();
}

void MainWindow::SaveProjectSession() {
    if (projectRoot_.empty()) {
        settings_->sessionFiles.clear();
        for (auto& e : editors_) {
            if (!e->file().empty()) settings_->sessionFiles.push_back(e->file().wstring());
        }
        return;
    }
    std::vector<std::wstring> open;
    for (auto& e : editors_) {
        if (IsUnder(e->file(), projectRoot_)) open.push_back(e->file().lexically_relative(projectRoot_).wstring());
    }
    std::vector<std::wstring> bps;
    for (const auto& [key, lines] : CollectBreakpoints()) {
        if (!IsUnder(key, projectRoot_) || lines.empty()) continue;
        std::vector<std::wstring> nums;
        for (int l : lines) nums.push_back(std::to_wstring(l));
        bps.push_back(fs::path(key).lexically_relative(Lower(NormalizePath(projectRoot_).wstring())).wstring() + L"*" +
                      Join(nums, L","));
    }
    std::vector<std::wstring> excluded;
    for (const std::wstring& key : excludedFromBuild_) {
        if (IsUnder(key, projectRoot_)) {
            excluded.push_back(fs::path(key).lexically_relative(Lower(NormalizePath(projectRoot_).wstring())).wstring());
        }
    }
    std::wstring text = L"main=" + (IsUnder(mainFile_, projectRoot_) ? mainFile_.lexically_relative(projectRoot_).wstring() : L"") +
                        L"\r\nopen=" + Join(open, L"|") + L"\r\nactive=" + std::to_wstring(active_) +
                        L"\r\nbreakpoints=" + Join(bps, L"|") + L"\r\nexclude=" + Join(excluded, L"|") + L"\r\n";
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
        return;
    }
    for (const std::wstring& f : settings_->sessionFiles) {
        if (FileExists(f)) OpenFile(f);
    }
}

void MainWindow::NewProject() {
    auto dir = PickFolder(L"Папка для нового проекта (можно создать новую)");
    if (!dir) return;
    fs::path main = *dir / L"main.asm";
    if (!fs::exists(main)) {
        std::ofstream out(main, std::ios::binary);
        out << kTemplate;
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
        else if (!folder && excludedFromBuild_.count(Project::Key(p))) name += L"   — не собирается";
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
    AppendMenuW(menu, MF_STRING, ID_TREE_NEW_FILE, L"Новый файл…");
    AppendMenuW(menu, MF_STRING, ID_TREE_NEW_FOLDER, L"Новая папка…");
    if (file) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TREE_OPEN, L"Открыть");
        AppendMenuW(menu, MF_STRING, ID_TREE_MAIN,
                    SamePath(target, mainFile_) ? L"Убрать отметку главного файла" : L"Сделать главным файлом (запуск)");
        if (Project::IsSourceFile(target)) {
            AppendMenuW(menu, MF_STRING, ID_TREE_EXCLUDE, excludedFromBuild_.count(Project::Key(target))
                                                               ? L"Собирать этот файл"
                                                               : L"Не собирать этот файл");
        }
    }
    if (!SamePath(target, projectRoot_)) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_TREE_RENAME, L"Переименовать…");
        AppendMenuW(menu, MF_STRING, ID_TREE_DELETE, L"Удалить в корзину");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TREE_REVEAL, L"Показать в проводнике");
    AppendMenuW(menu, MF_STRING, ID_REFRESH_TREE, L"Обновить");
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
}
