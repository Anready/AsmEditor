#include "mw_internal.h"

// ---- completion -------------------------------------------------------------------------------------------------

// ---- find usages ------------------------------------------------------------------------------------------------

void MainWindow::ClearUsageHighlight() {
    if (!usageEditor_) return;
    bool alive = std::any_of(editors_.begin(), editors_.end(), [&](const auto& e) { return e.get() == usageEditor_; });
    if (alive) {
        usageEditor_->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorFind);
        usageEditor_->Call(SCI_INDICATORCLEARRANGE, 0, usageEditor_->Call(SCI_GETLENGTH));
    }
    usageEditor_ = nullptr;
}

// The user-defined name under the caret and, for a local label (.loop), the global label it belongs to.
bool MainWindow::SymbolUnderCaret(Editor* ed, Sci_Position pos, std::string& word, std::string& scope) {
    word = ed->WordAt(pos);
    while (!word.empty() && word.back() == '.') word.pop_back();
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, pos > 0 && ed->Call(SCI_GETCHARAT, pos) == ' ' ? pos - 1 : pos));
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0])) || style == T_REGISTER ||
        style == T_INSTRUCTION || style == T_KEYWORD || style == T_DIRECTIVE || style == T_COMMENT || style == T_STRING) {
        std::string tip = "Поставьте курсор на метку, константу или макрос";
        ed->Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(tip.c_str()));
        return false;
    }
    scope.clear();
    if (word[0] == '.' && word.rfind("..", 0) != 0) {
        int caretLine = static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, pos)) + 1;
        for (const auto& s : Completion::ScanSymbols(ed->Text())) {
            if (s.line > caretLine) break;
            if (s.kind == Completion::Kind::Label && !s.declaration && s.name[0] != '.') scope = s.name;
        }
    }
    return true;
}

// Texts to search: the current file, other open tabs (current text) and the project's source files on disk.
// A local label is searched only in its own file.
std::vector<MainWindow::UsageSource> MainWindow::UsageSources(Editor* ed, bool local) {
    std::vector<UsageSource> sources{{ed->file(), ed, ed->Text()}};
    if (local) return sources;
    std::set<std::wstring> seen;
    if (!ed->file().empty()) seen.insert(Debugger::Key(ed->file()));
    for (const auto& e : editors_) {
        if (e.get() == ed || e->file().empty()) continue;
        if (seen.insert(Debugger::Key(e->file())).second) sources.push_back({e->file(), e.get(), e->Text()});
    }
    static const std::set<std::wstring> kSourceExt = {L".asm", L".inc", L".mac", L".s", L".nasm"};
    for (const fs::path& p : treePaths_) {
        if (!kSourceExt.count(Lower(p.extension().wstring())) || !seen.insert(Debugger::Key(p)).second) continue;
        std::error_code ec;
        if (fs::file_size(p, ec) > 4'000'000 || ec) continue;
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        sources.push_back({p, nullptr, ss.str()});
    }
    return sources;
}

// Lists every use of the name under the caret in the console ("file:line: code", double-click jumps there)
// and highlights the uses in the current file.
void MainWindow::FindUsages(Editor* ed, Sci_Position pos) {
    std::string word, scope;
    if (!SymbolUnderCaret(ed, pos, word, scope)) return;
    bool local = word[0] == '.' && word.rfind("..", 0) != 0;
    std::vector<UsageSource> sources = UsageSources(ed, local);

    struct Hit {
        const UsageSource* source;
        Completion::Occurrence occ;
    };
    std::vector<Hit> hits;
    size_t files = 0;
    for (const UsageSource& src : sources) {
        auto found = Completion::FindOccurrences(src.text, word, scope);
        if (!found.empty()) files++;
        for (auto& o : found) hits.push_back({&src, std::move(o)});
    }

    // Highlight the uses in the current file.
    ClearUsageHighlight();
    usageEditor_ = ed;
    ed->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorFind);
    for (const Hit& h : hits) {
        if (h.source->editor != ed) continue;
        Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, h.occ.line - 1);
        ed->Call(SCI_INDICATORFILLRANGE, lineStart + h.occ.column, h.occ.length);
    }

    Sci_Position headerLine = console_->Call(SCI_GETLINECOUNT) - 1;
    std::string what = local && !scope.empty() ? word + "» в " + scope : word + "»";
    Print("\nИспользования «" + what + ": " + std::to_string(hits.size()) + " в " + std::to_string(files) +
              (files == 1 ? " файле" : " файлах") + " — двойной клик открывает место\n",
          Out::Info);
    for (const Hit& h : hits) {
        fs::path shown = h.source->file;
        if (!shown.empty() && IsUnder(shown, projectRoot_)) shown = NormalizePath(shown).lexically_relative(NormalizePath(projectRoot_));
        std::string where = (shown.empty() ? Narrow(ed->Title()) : Narrow(shown.wstring())) + ":" + std::to_string(h.occ.line) + ":";
        std::string code = h.occ.lineText;
        size_t first = code.find_first_not_of(" \t");
        code = first == std::string::npos ? std::string() : code.substr(first);
        Print(where + " ", Out::Input);
        Print(code + (h.occ.definition ? "" : "\n"), Out::Normal);
        if (h.occ.definition) Print("   ← определение\n", Out::Success);
    }
    if (hits.empty()) Print("ничего не найдено\n", Out::Warning);
    console_->Call(SCI_SETFIRSTVISIBLELINE, headerLine);
}

// F2: renames a label, constant or macro everywhere in the project. Changed files are left unsaved (and open),
// so every edit can be reviewed and undone.
void MainWindow::Rename(Editor* ed, Sci_Position pos) {
    std::string word, scope;
    if (!SymbolUnderCaret(ed, pos, word, scope)) return;
    bool local = word[0] == '.' && word.rfind("..", 0) != 0;
    if (local && scope.empty()) {
        ShowMessage(hwnd_, L"Локальная метка вне глобальной метки — переименуйте её вручную.", false);
        return;
    }
    auto answer = InputBox(hwnd_, L"Переименование", L"Новое имя для «" + Widen(word) + L"»:", Widen(word));
    if (!answer) return;
    std::string name = Narrow(Trim(*answer));
    if (name == word) return;

    std::wstring problem;
    if (!Completion::IsValidName(name)) {
        problem = L"«" + Widen(name) + L"» — недопустимое имя. Можно буквы, цифры, _ . ? @ $ # ~; начинается не с цифры.";
    } else if (Completion::IsReservedWord(name)) {
        problem = L"«" + Widen(name) + L"» — зарезервированное слово NASM (регистр, инструкция или директива).";
    } else if (local != (name[0] == '.' && name.rfind("..", 0) != 0)) {
        problem = local ? L"Локальная метка должна начинаться с точки." : L"Имя глобального символа не может начинаться с точки.";
    }
    std::vector<UsageSource> sources = UsageSources(ed, local);
    if (problem.empty()) {  // the new name must not already be defined (in the same scope for a local label)
        for (const UsageSource& src : sources) {
            for (const auto& s : Completion::ScanSymbols(src.text)) {
                if (s.name != name || s.declaration) continue;
                if (local && s.scope != scope) continue;
                std::wstring where = src.file.empty() ? ed->Title() : src.file.filename().wstring();
                problem = L"«" + Widen(name) + L"» уже определено: " + where + L", строка " + std::to_wstring(s.line) + L".";
                break;
            }
            if (!problem.empty()) break;
        }
    }
    if (!problem.empty()) {
        ShowMessage(hwnd_, problem);
        return;
    }

    // Open every affected file (from disk text first, so unchanged files stay closed), then edit the tab's text.
    std::vector<Editor*> targets;
    for (const UsageSource& src : sources) {
        if (Completion::FindOccurrences(src.text, word, scope, !local).empty()) continue;
        Editor* target = src.editor ? src.editor : OpenFile(src.file);
        if (target) targets.push_back(target);
    }
    size_t total = 0;
    for (Editor* target : targets) {
        auto found = Completion::FindOccurrences(target->Text(), word, scope, !local);
        target->Call(SCI_BEGINUNDOACTION);
        for (auto it = found.rbegin(); it != found.rend(); ++it) {  // back to front: earlier offsets stay valid
            std::string replacement = it->qualified && local ? scope + name : name;
            Sci_Position at = target->Call(SCI_POSITIONFROMLINE, it->line - 1) + it->column;
            target->Call(SCI_SETTARGETRANGE, at, at + it->length);
            target->Call(SCI_REPLACETARGET, replacement.size(), reinterpret_cast<sptr_t>(replacement.c_str()));
        }
        target->Call(SCI_ENDUNDOACTION);
        total += found.size();
    }
    for (size_t i = 0; i < editors_.size(); i++) {
        if (editors_[i].get() == ed) Activate(static_cast<int>(i));
    }
    InvalidateChrome();
    Print("\nПереименовано «" + word + "» → «" + name + "»: " + std::to_string(total) + " замен в " +
              std::to_string(targets.size()) + (targets.size() == 1 ? " файле" : " файлах") +
              ". Файлы не сохранены: Ctrl+Z — отменить в файле, «Сохранить всё» — сохранить.\n",
          Out::Success);
}

// ---- go to definition -------------------------------------------------------------------------------------------

// Symbols of every other source file of the project (open tabs use their current text), with their files.
std::vector<MainWindow::SymbolHit> MainWindow::ProjectSymbols(const Editor* current) {
    std::vector<SymbolHit> out;
    std::set<std::wstring> open;
    for (const auto& e : editors_) {
        if (!e->file().empty()) open.insert(Debugger::Key(e->file()));
        if (e.get() == current) continue;
        for (auto& sym : Completion::ScanSymbols(e->Text())) out.push_back({e.get(), e->file(), std::move(sym)});
    }
    static const std::set<std::wstring> kSourceExt = {L".asm", L".inc", L".mac", L".s", L".nasm"};
    for (const fs::path& p : treePaths_) {
        if (!kSourceExt.count(Lower(p.extension().wstring()))) continue;
        std::wstring key = Debugger::Key(p);
        if (open.count(key)) continue;
        std::error_code ec;
        auto time = fs::last_write_time(p, ec);
        if (ec || fs::file_size(p, ec) > 1'000'000) continue;
        auto it = symbolCache_.find(key);
        if (it == symbolCache_.end() || it->second.time != time) {
            std::ifstream in(p, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            it = symbolCache_.insert_or_assign(key, CachedSymbols{time, Completion::ScanSymbols(ss.str()), p}).first;
        }
        for (const auto& sym : it->second.symbols) out.push_back({nullptr, it->second.path, sym});
    }
    return out;
}

std::vector<Completion::Symbol> MainWindow::OtherFileSymbols(const Editor* current) {
    std::vector<Completion::Symbol> out;
    for (auto& hit : ProjectSymbols(current)) {
        if (!hit.symbol.name.empty() && hit.symbol.name[0] != '.') out.push_back(std::move(hit.symbol));  // locals stay in their file
    }
    return out;
}

// Ctrl+click on the file name of a %include line opens that file.
bool MainWindow::OpenIncludeAt(Editor* ed, Sci_Position pos) {
    Sci_Position line = ed->Call(SCI_LINEFROMPOSITION, pos);
    Sci_Position start = ed->Call(SCI_POSITIONFROMLINE, line), end = ed->Call(SCI_GETLINEENDPOSITION, line);
    std::string text(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, start, end - start)), end - start);
    size_t i = text.find_first_not_of(" \t");
    if (i == std::string::npos || text.size() - i < 8 || _strnicmp(text.c_str() + i, "%include", 8) != 0) return false;
    size_t open = text.find_first_of("\"'<", i + 8);
    if (open == std::string::npos) return false;
    size_t close = text.find(text[open] == '<' ? '>' : text[open], open + 1);
    if (close == std::string::npos) return false;
    Sci_Position col = pos - start;
    if (col < static_cast<Sci_Position>(open) || col > static_cast<Sci_Position>(close)) return false;
    fs::path name = Widen(text.substr(open + 1, close - open - 1));
    std::vector<fs::path> dirs;
    if (!ed->file().empty()) dirs.push_back(ed->file().parent_path());
    if (!projectRoot_.empty()) dirs.push_back(projectRoot_);
    for (const fs::path& d : dirs) {
        if (FileExists((d / name).wstring())) {
            PushNavigation();
            OpenFile(d / name);
            return true;
        }
    }
    std::string tip = "Файл не найден: " + Narrow(name.wstring());
    ed->Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(tip.c_str()));
    return true;
}

void MainWindow::GoToDefinition(Editor* ed, Sci_Position pos) {
    if (OpenIncludeAt(ed, pos)) return;
    std::string word = ed->WordAt(pos);
    while (!word.empty() && word.back() == '.') word.pop_back();
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0]))) return;
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, pos > 0 && ed->Call(SCI_GETCHARAT, pos) == ' ' ? pos - 1 : pos));
    if (style == T_REGISTER || style == T_INSTRUCTION || style == T_KEYWORD || style == T_COMMENT || style == T_STRING) {
        return;  // built-in words and text have no definition
    }
    int caretLine = static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, pos)) + 1;
    std::vector<Completion::Symbol> own = Completion::ScanSymbols(ed->Text());

    // Local label (.loop): belongs to the last global label above the cursor.
    bool local = word[0] == '.' && word.rfind("..", 0) != 0;
    std::string scope;
    size_t dot = word.find('.', 1);
    if (local) {
        for (const auto& s : own) {
            if (s.line > caretLine) break;
            if (s.kind == Completion::Kind::Label && !s.declaration && s.name[0] != '.') scope = s.name;
        }
    } else if (dot != std::string::npos && word.rfind("..", 0) != 0) {  // fully qualified: main.loop
        auto qualified = [&](const Completion::Symbol& s) {
            return s.name == word.substr(dot) && s.scope == word.substr(0, dot);
        };
        for (const auto& s : own) {
            if (qualified(s)) return JumpTo({ed, ed->file(), s});
        }
        for (const auto& hit : ProjectSymbols(ed)) {
            if (qualified(hit.symbol)) return JumpTo(hit);
        }
    }

    auto isDefinition = [&](const Completion::Symbol& s) {
        return s.name == word && !s.declaration && (s.kind == Completion::Kind::Label || s.kind == Completion::Kind::Macro);
    };
    if (local) {
        const Completion::Symbol* best = nullptr;
        for (const auto& s : own) {
            if (!isDefinition(s)) continue;
            if (s.scope == scope) {
                best = &s;
                break;
            }
            if (!best) best = &s;
        }
        if (best) return JumpTo({ed, ed->file(), *best});
    } else {
        for (const auto& s : own) {
            if (isDefinition(s)) return JumpTo({ed, ed->file(), s});
        }
        std::vector<SymbolHit> others = ProjectSymbols(ed);
        for (const auto& hit : others) {
            if (isDefinition(hit.symbol)) return JumpTo(hit);
        }
        // Not defined in the project: show where it is declared (extern/global).
        auto declared = [&](const Completion::Symbol& s) { return s.name == word && s.declaration; };
        const SymbolHit* decl = nullptr;
        SymbolHit ownDecl{};
        for (const auto& s : own) {
            if (declared(s)) {
                ownDecl = {ed, ed->file(), s};
                decl = &ownDecl;
                break;
            }
        }
        if (!decl) {
            for (const auto& hit : others) {
                if (declared(hit.symbol)) {
                    decl = &hit;
                    break;
                }
            }
        }
        if (decl) {
            JumpTo(*decl);
            Editor* now = Active();
            if (now && decl->symbol.kind == Completion::Kind::Function) {
                std::string tip = word + " — внешний символ: определён вне проекта (библиотека или DLL)";
                now->Call(SCI_CALLTIPSHOW, now->Call(SCI_GETCURRENTPOS), reinterpret_cast<sptr_t>(tip.c_str()));
            }
            return;
        }
    }
    std::string tip = "Определение «" + word + "» не найдено";
    ed->Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(tip.c_str()));
}

void MainWindow::JumpTo(const SymbolHit& hit) {
    PushNavigation();
    Editor* ed = nullptr;
    if (hit.editor) {
        for (size_t i = 0; i < editors_.size(); i++) {
            if (editors_[i].get() == hit.editor) Activate(static_cast<int>(i));
        }
        ed = hit.editor;
    } else {
        ed = OpenFile(hit.file);
    }
    if (!ed) return;
    ed->GotoLine(hit.symbol.line);
    // Select the name on its line.
    Sci_Position line = hit.symbol.line - 1;
    Sci_Position start = ed->Call(SCI_POSITIONFROMLINE, line), end = ed->Call(SCI_GETLINEENDPOSITION, line);
    std::string text(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, start, end - start)), end - start);
    size_t at = text.find(hit.symbol.name);
    if (at != std::string::npos) ed->Call(SCI_SETSEL, start + at, start + at + hit.symbol.name.size());
}

MainWindow::NavLocation MainWindow::CurrentLocation() {
    Editor* ed = Active();
    if (!ed) return {{}, 0, 0};
    return {ed->file(), ed->id(), ed->Call(SCI_GETCURRENTPOS)};
}

void MainWindow::PushNavigation() {
    if (!Active()) return;
    navBack_.push_back(CurrentLocation());
    if (navBack_.size() > 100) navBack_.erase(navBack_.begin());
    navForward_.clear();
}

// Alt+Left / Alt+Right: back and forward through the places visited by "go to definition".
void MainWindow::Navigate(bool back) {
    auto& from = back ? navBack_ : navForward_;
    auto& to = back ? navForward_ : navBack_;
    while (!from.empty()) {
        NavLocation loc = from.back();
        from.pop_back();
        Editor* ed = nullptr;
        if (!loc.file.empty() && FileExists(loc.file.wstring())) {
            ed = OpenFile(loc.file);
        } else if (Editor* e = FindById(loc.editorId)) {
            for (size_t i = 0; i < editors_.size(); i++) {
                if (editors_[i].get() == e) Activate(static_cast<int>(i));
            }
            ed = e;
        }
        if (!ed) continue;  // the tab was closed: try the next entry
        if (Active()) to.push_back(CurrentLocation());
        ed->Call(SCI_GOTOPOS, std::min<Sci_Position>(loc.pos, ed->Call(SCI_GETLENGTH)));
        ed->Call(SCI_SCROLLCARET);
        SetFocus(ed->hwnd());
        return;
    }
    MessageBeep(MB_OK);
}

void MainWindow::ClearLinkHover() {
    if (!linkEditor_) return;
    bool alive = std::any_of(editors_.begin(), editors_.end(), [&](const auto& e) { return e.get() == linkEditor_; });
    if (alive) {  // the tab may have been closed meanwhile
        linkEditor_->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorLink);
        linkEditor_->Call(SCI_INDICATORCLEARRANGE, linkStart_, linkEnd_ - linkStart_);
    }
    linkEditor_ = nullptr;
}

// Ctrl held over a name: underline it like a link.
void MainWindow::UpdateLinkHover(Editor* ed, bool ctrl) {
    Sci_Position start = 0, end = 0;
    bool link = false;
    if (ctrl) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(ed->hwnd(), &pt);
        Sci_Position pos = ed->Call(SCI_POSITIONFROMPOINTCLOSE, pt.x, pt.y);
        if (pos >= 0 && ed->WordRange(pos, start, end)) {
            int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, start));
            char first = static_cast<char>(ed->Call(SCI_GETCHARAT, start));
            link = (style == T_DEFAULT || style == T_LABEL) && !isdigit(static_cast<unsigned char>(first));
            if (!link && style == T_STRING) {  // file name in %include "..."
                Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, ed->Call(SCI_LINEFROMPOSITION, pos));
                link = ed->Call(SCI_GETSTYLEAT, ed->Call(SCI_GETLINEINDENTPOSITION, ed->Call(SCI_LINEFROMPOSITION, lineStart))) ==
                       T_PREPROCESSOR;
            }
        }
    }
    if (link && linkEditor_ == ed && linkStart_ == start && linkEnd_ == end) return;
    ClearLinkHover();
    if (!link) return;
    linkEditor_ = ed;
    linkStart_ = start;
    linkEnd_ = end;
    ed->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorLink);
    ed->Call(SCI_INDICATORFILLRANGE, start, end - start);
}

LRESULT CALLBACK MainWindow::EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    Editor* ed = self->FindByHwnd(hwnd);
    if (ed) {
        switch (msg) {
        case WM_MOUSEMOVE:
            self->UpdateLinkHover(ed, (wp & MK_CONTROL) != 0);
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
            if (wp == VK_CONTROL) self->UpdateLinkHover(ed, msg == WM_KEYDOWN);
            break;
        case WM_KILLFOCUS:
        case WM_MOUSELEAVE:
            self->ClearLinkHover();
            break;
        case WM_SETCURSOR:
            if (self->linkEditor_ == ed && LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            if (wp & MK_CONTROL) {  // Ctrl+click: go to definition (instead of Scintilla's word selection)
                Sci_Position pos = ed->Call(SCI_POSITIONFROMPOINTCLOSE, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                self->ClearLinkHover();
                if (pos >= 0) self->GoToDefinition(ed, pos);
                return 0;
            }
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Shows (or refreshes) the completion list for the word before the caret.
void MainWindow::Complete(Editor* ed, bool explicitRequest) {
    Sci_Position pos = ed->Call(SCI_GETCURRENTPOS);
    Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, ed->Call(SCI_LINEFROMPOSITION, pos));
    std::string line(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, lineStart, pos - lineStart)),
                     pos - lineStart);
    size_t start = line.size();
    auto wordChar = [](unsigned char c) {
        return isalnum(c) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' || c == '#' || c == '~' || c >= 0x80;
    };
    while (start > 0 && wordChar(static_cast<unsigned char>(line[start - 1]))) start--;
    if (start > 0 && line[start - 1] == '%') start--;
    std::string prefix = line.substr(start);
    bool active = ed->Call(SCI_AUTOCACTIVE) != 0;
    if (!explicitRequest && !active && prefix.size() < 2) return;  // pops up by itself after two characters
    if (!prefix.empty() && isdigit(static_cast<unsigned char>(prefix[0]))) return;

    std::vector<Completion::Symbol> symbols = Completion::ScanSymbols(ed->Text());
    for (auto& sym : OtherFileSymbols(ed)) symbols.push_back(std::move(sym));
    std::string list = Completion::BuildList(line.substr(0, start), prefix, symbols);
    if (list.empty()) {
        if (active) ed->Call(SCI_AUTOCCANCEL);
        return;
    }
    ed->Call(SCI_AUTOCSHOW, prefix.size(), reinterpret_cast<sptr_t>(list.c_str()));
    // The list is a popup created by Scintilla: give it (and its scrollbar) the dark theme.
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM) -> BOOL {
        wchar_t cls[32];
        if (GetClassNameW(w, cls, 32) && wcscmp(cls, L"ListBoxX") == 0 && !GetPropW(w, L"AsmDark")) {
            SetPropW(w, L"AsmDark", reinterpret_cast<HANDLE>(1));
            ApplyDarkControl(w);
            EnumChildWindows(w, [](HWND child, LPARAM) -> BOOL {
                ApplyDarkControl(child);
                return TRUE;
            }, 0);
        }
        return TRUE;
    }, 0);
}

// ---- find & replace ---------------------------------------------------------------------------------------------

void MainWindow::ShowFind(bool replace) {
    Editor* ed = Active();
    if (ed) {
        Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
        if (e > s && e - s < 200) {
            std::string sel(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, s, e - s)), e - s);
            if (sel.find('\n') == std::string::npos) wcsncpy_s(findText_, Widen(sel).c_str(), _TRUNCATE);
        }
    }
    if (findDialog_) DestroyWindow(findDialog_);
    findData_ = {sizeof(findData_)};
    findData_.hwndOwner = hwnd_;
    findData_.lpstrFindWhat = findText_;
    findData_.wFindWhatLen = static_cast<WORD>(std::size(findText_));
    findData_.lpstrReplaceWith = replaceText_;
    findData_.wReplaceWithLen = static_cast<WORD>(std::size(replaceText_));
    findData_.Flags = lastFindFlags_ & (FR_DOWN | FR_MATCHCASE | FR_WHOLEWORD);
    findDialog_ = replace ? ReplaceTextW(&findData_) : FindTextW(&findData_);
}

bool MainWindow::FindNext(bool forward) {
    Editor* ed = Active();
    if (!ed || lastFind_.empty()) return false;
    std::string q = Narrow(lastFind_);
    int flags = (lastFindFlags_ & FR_MATCHCASE ? SCFIND_MATCHCASE : 0) | (lastFindFlags_ & FR_WHOLEWORD ? SCFIND_WHOLEWORD : 0);
    ed->Call(SCI_SETSEARCHFLAGS, flags);
    Sci_Position len = ed->Call(SCI_GETLENGTH);
    auto search = [&](Sci_Position from, Sci_Position to) {
        ed->Call(SCI_SETTARGETRANGE, from, to);
        return ed->Call(SCI_SEARCHINTARGET, q.size(), reinterpret_cast<sptr_t>(q.c_str()));
    };
    Sci_Position r = forward ? search(ed->Call(SCI_GETSELECTIONEND), len) : search(ed->Call(SCI_GETSELECTIONSTART), 0);
    if (r < 0) r = forward ? search(0, len) : search(len, 0);
    if (r < 0) {
        MessageBeep(MB_ICONASTERISK);
        return false;
    }
    ed->Call(SCI_SETSEL, ed->Call(SCI_GETTARGETSTART), ed->Call(SCI_GETTARGETEND));
    ed->Call(SCI_SCROLLCARET);
    return true;
}

void MainWindow::OnFindMessage(FINDREPLACEW* fr) {
    if (fr->Flags & FR_DIALOGTERM) {
        findDialog_ = nullptr;
        return;
    }
    lastFind_ = fr->lpstrFindWhat;
    lastFindFlags_ = fr->Flags;
    Editor* ed = Active();
    if (!ed) return;
    if (fr->Flags & FR_FINDNEXT) {
        FindNext((fr->Flags & FR_DOWN) != 0);
    } else if (fr->Flags & FR_REPLACE) {
        std::string q = Narrow(lastFind_), rep = Narrow(fr->lpstrReplaceWith);
        Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
        std::string sel(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, s, e - s)), e - s);
        bool match = (fr->Flags & FR_MATCHCASE) ? sel == q : _stricmp(sel.c_str(), q.c_str()) == 0;
        if (match && !sel.empty()) ed->Call(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(rep.c_str()));
        FindNext(true);
    } else if (fr->Flags & FR_REPLACEALL) {
        std::string q = Narrow(lastFind_), rep = Narrow(fr->lpstrReplaceWith);
        int flags = (fr->Flags & FR_MATCHCASE ? SCFIND_MATCHCASE : 0) | (fr->Flags & FR_WHOLEWORD ? SCFIND_WHOLEWORD : 0);
        ed->Call(SCI_SETSEARCHFLAGS, flags);
        ed->Call(SCI_BEGINUNDOACTION);
        int count = 0;
        Sci_Position pos = 0;
        for (;;) {
            ed->Call(SCI_SETTARGETRANGE, pos, ed->Call(SCI_GETLENGTH));
            if (ed->Call(SCI_SEARCHINTARGET, q.size(), reinterpret_cast<sptr_t>(q.c_str())) < 0) break;
            Sci_Position n = ed->Call(SCI_REPLACETARGET, rep.size(), reinterpret_cast<sptr_t>(rep.c_str()));
            pos = ed->Call(SCI_GETTARGETSTART) + n;
            count++;
        }
        ed->Call(SCI_ENDUNDOACTION);
        ShowMessage(findDialog_ ? findDialog_ : hwnd_, L"Заменено: " + std::to_wstring(count), false);
    }
}
