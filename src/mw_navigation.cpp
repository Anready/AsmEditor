#include "mw_internal.h"
#include "hints.h"

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
bool MainWindow::SymbolUnderCaret(Editor* ed, Sci_Position pos, std::string& word, std::string& scope, bool quiet) {
    word = ed->WordAt(pos);
    while (!word.empty() && word.back() == '.') word.pop_back();
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, pos > 0 && ed->Call(SCI_GETCHARAT, pos) == ' ' ? pos - 1 : pos));
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0])) || style == T_REGISTER ||
        style == T_INSTRUCTION || style == T_KEYWORD || style == T_DIRECTIVE || style == T_COMMENT || style == T_STRING) {
        if (!quiet) {
            std::string tip = Tr(std::string("Поставьте курсор на метку, константу или макрос"));
            ed->Call(SCI_CALLTIPSHOW, pos, reinterpret_cast<sptr_t>(tip.c_str()));
        }
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

    ClearPane(Pane::Search);
    ShowPane(Pane::Search);
    std::string what = local && !scope.empty() ? word + "» (" + scope + ")" : word + "»";
    PrintTo(Pane::Search, Tr(std::string("Использования")) + " «" + what + ": " + std::to_string(hits.size()) + ", " +
                              Tr(std::string("в файлах:")) + " " + std::to_string(files) + " — " +
                              Tr(std::string("двойной клик открывает место")) + "\n",
            Out::Info);
    for (const Hit& h : hits) {
        fs::path shown = h.source->file;
        if (!shown.empty() && IsUnder(shown, projectRoot_)) shown = NormalizePath(shown).lexically_relative(NormalizePath(projectRoot_));
        std::string where = (shown.empty() ? Narrow(ed->Title()) : Narrow(shown.wstring())) + ":" + std::to_string(h.occ.line) + ":";
        std::string code = h.occ.lineText;
        size_t first = code.find_first_not_of(" \t");
        code = first == std::string::npos ? std::string() : code.substr(first);
        PrintTo(Pane::Search, where + " ", Out::Input);
        PrintTo(Pane::Search, code + (h.occ.definition ? "" : "\n"), Out::Normal);
        if (h.occ.definition) PrintTo(Pane::Search, "   ← " + Tr(std::string("определение")) + "\n", Out::Success);
    }
    if (hits.empty()) PrintTo(Pane::Search, Tr(std::string("ничего не найдено\n")), Out::Warning);
    View(Pane::Search).Call(SCI_SETFIRSTVISIBLELINE, 0);
}

// F2: renames a label, constant or macro everywhere in the project. Changed files are left unsaved (and open),
// so every edit can be reviewed and undone.
void MainWindow::Rename(Editor* ed, Sci_Position pos) {
    std::string word, scope;
    if (!SymbolUnderCaret(ed, pos, word, scope)) return;
    bool local = word[0] == '.' && word.rfind("..", 0) != 0;
    if (local && scope.empty()) {
        ShowMessage(hwnd_, Tr(L"Локальная метка вне глобальной метки — переименуйте её вручную."), false);
        return;
    }
    auto answer = InputBox(hwnd_, Tr(L"Переименование"), Tr(L"Новое имя для ") + Widen(word) + L":", Widen(word));
    if (!answer) return;
    std::string name = Narrow(Trim(*answer));
    if (name == word) return;

    std::wstring problem;
    if (!Completion::IsValidName(name)) {
        problem = Widen(name) + Tr(L": недопустимое имя. Можно буквы, цифры, _ . ? @ $ # ~; начинается не с цифры.");
    } else if (Completion::IsReservedWord(name)) {
        problem = Widen(name) + Tr(L": зарезервированное слово NASM (регистр, инструкция или директива).");
    } else if (local != (name[0] == '.' && name.rfind("..", 0) != 0)) {
        problem = local ? Tr(L"Локальная метка должна начинаться с точки.") : Tr(L"Имя глобального символа не может начинаться с точки.");
    }
    std::vector<UsageSource> sources = UsageSources(ed, local);
    if (problem.empty()) {  // the new name must not already be defined (in the same scope for a local label)
        for (const UsageSource& src : sources) {
            for (const auto& s : Completion::ScanSymbols(src.text)) {
                if (s.name != name || s.declaration) continue;
                if (local && s.scope != scope) continue;
                std::wstring where = src.file.empty() ? ed->Title() : src.file.filename().wstring();
                problem = Widen(name) + Tr(L": уже определено в ") + where + L":" + std::to_wstring(s.line) + L".";
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
    Print("\n" + Tr(std::string("Переименовано: ")) + word + " → " + name + " (" + Tr(std::string("замен: ")) +
              std::to_string(total) + ", " + Tr(std::string("файлов: ")) + std::to_string(targets.size()) + "). " +
              Tr(std::string("Файлы не сохранены: Ctrl+Z — отменить в файле, «Сохранить всё» — сохранить.")) + "\n",
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
    std::string tip = Tr(std::string("Файл не найден: ")) + Narrow(name.wstring());
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
                std::string tip = word + Tr(std::string(": внешний символ, определён вне проекта (библиотека или DLL)"));
                now->Call(SCI_CALLTIPSHOW, now->Call(SCI_GETCURRENTPOS), reinterpret_cast<sptr_t>(tip.c_str()));
            }
            return;
        }
    }
    std::string tip = Tr(std::string("Определение не найдено: ")) + word;
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
            if (GetKeyState(VK_MENU) < 0 && !(wp & MK_SHIFT)) {  // Alt+click: one more caret
                Sci_Position pos = ed->Call(SCI_POSITIONFROMPOINT, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (pos >= 0) {
                    SetFocus(hwnd);
                    ed->Call(SCI_ADDSELECTION, pos, pos);
                    return 0;
                }
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
    // At the start of a statement the snippets are offered too ("loop…").
    if (!prefix.empty() && prefix[0] != '%' && line.substr(0, start).find_first_not_of(" \t") == std::string::npos) {
        for (const Snippets::Snippet& s : Snippets::All()) {
            if (_strnicmp(s.name, prefix.c_str(), prefix.size()) != 0) continue;
            if (!list.empty()) list += ' ';
            list += std::string(s.name) + "…?" + std::to_string(static_cast<int>(Completion::Kind::Snippet));
        }
    }
    if (list.empty()) {
        if (active) ed->Call(SCI_AUTOCCANCEL);
        return;
    }
    ed->Call(SCI_AUTOCSETORDER, SC_ORDER_PERFORMSORT);
    ed->Call(SCI_AUTOCSHOW, prefix.size(), reinterpret_cast<sptr_t>(list.c_str()));
}

// ---- snippets, formatting, carets, occurrences, folding -------------------------------------------------------------

void MainWindow::ShowSnippets(Editor* ed) {
    std::string list;
    for (const Snippets::Snippet& s : Snippets::All()) {
        if (!list.empty()) list += ' ';
        list += std::string(s.name) + "?" + std::to_string(static_cast<int>(Completion::Kind::Snippet));
    }
    ed->Call(SCI_AUTOCSETORDER, SC_ORDER_PERFORMSORT);
    ed->Call(SCI_USERLISTSHOW, 1, reinterpret_cast<sptr_t>(list.c_str()));
    // A hint with what each snippet is.
    std::string tip = Tr(std::string("Шаблоны кода:"));
    for (const Snippets::Snippet& s : Snippets::All()) tip += "\n" + std::string(s.name) + " — " + Tr(std::string(s.title));
    ed->Call(SCI_CALLTIPSHOW, ed->Call(SCI_GETCURRENTPOS), reinterpret_cast<sptr_t>(tip.c_str()));
}

// Replaces the current line's leading blanks by the snippet and puts the caret at its "|" mark.
void MainWindow::InsertSnippet(Editor* ed, const std::string& name) {
    const Snippets::Snippet* snippet = nullptr;
    for (const Snippets::Snippet& s : Snippets::All()) {
        if (name == s.name) snippet = &s;
    }
    if (!snippet) return;
    ed->Call(SCI_CALLTIPCANCEL);
    std::string eol = ed->Call(SCI_GETEOLMODE) == SC_EOL_LF ? "\n" : "\r\n";
    size_t caret = 0;
    std::string text = Snippets::Expand(*snippet, eol, caret);
    Sci_Position line = ed->Call(SCI_LINEFROMPOSITION, ed->Call(SCI_GETCURRENTPOS));
    Sci_Position start = ed->Call(SCI_POSITIONFROMLINE, line), end = ed->Call(SCI_GETLINEENDPOSITION, line);
    std::string current = ed->LineText(line);
    bool blank = current.find_first_not_of(" \t") == std::string::npos;
    ed->Call(SCI_BEGINUNDOACTION);
    if (blank) {
        ed->Call(SCI_SETTARGETRANGE, start, end);
        ed->Call(SCI_REPLACETARGET, text.size(), reinterpret_cast<sptr_t>(text.c_str()));
    } else {  // the line has code: insert below it
        text = eol + text;
        caret += eol.size();
        start = end;
        ed->Call(SCI_INSERTTEXT, start, reinterpret_cast<sptr_t>(text.c_str()));
    }
    ed->Call(SCI_ENDUNDOACTION);
    ed->Call(SCI_GOTOPOS, start + static_cast<Sci_Position>(caret));
    SetFocus(ed->hwnd());
}

// Ctrl+Alt+L: aligns mnemonics, operands and comments (the selected lines, or the whole file).
void MainWindow::FormatCode(Editor* ed) {
    AsmFormat::Options o{settings_->formatIndent, settings_->formatOperands, settings_->formatComments};
    std::string eol = ed->Call(SCI_GETEOLMODE) == SC_EOL_LF ? "\n" : "\r\n";
    Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
    Sci_Position first = 0, last = ed->Call(SCI_GETLINECOUNT) - 1;
    if (e > s) {
        first = ed->Call(SCI_LINEFROMPOSITION, s);
        last = ed->Call(SCI_LINEFROMPOSITION, e);
        if (last > first && ed->Call(SCI_POSITIONFROMLINE, last) == e) last--;
    }
    Sci_Position from = ed->Call(SCI_POSITIONFROMLINE, first), to = ed->Call(SCI_GETLINEENDPOSITION, last);
    std::string text(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, from, to - from)), to - from);
    std::string formatted = AsmFormat::Format(text, o, eol);
    if (formatted == text) return;
    Sci_Position caretLine = ed->Call(SCI_LINEFROMPOSITION, ed->Call(SCI_GETCURRENTPOS));
    Sci_Position top = ed->Call(SCI_GETFIRSTVISIBLELINE);
    ed->Call(SCI_SETTARGETRANGE, from, to);
    ed->Call(SCI_REPLACETARGET, formatted.size(), reinterpret_cast<sptr_t>(formatted.c_str()));
    ed->Call(SCI_GOTOPOS, ed->Call(SCI_GETLINEENDPOSITION, caretLine));
    ed->Call(SCI_SETFIRSTVISIBLELINE, top);
}

// Alt+Enter: a menu of fixes for the problems on the caret line (missing extern, stack space, saving rbx...).
void MainWindow::QuickFix(Editor* ed) {
    int line = ed->CaretLine();
    std::vector<Diagnostic> here;
    if (const auto* live = ed->DiagnosticsAt(line)) here = *live;
    for (const Diagnostic& d : buildDiagnostics_) {
        if (d.line == line && !ed->file().empty() && SamePath(d.file, ed->file())) here.push_back(d);
    }
    std::string text = ed->Text();
    std::vector<Analyzer::Fix> fixes;
    for (const Diagnostic& d : here) {
        if (d.code.empty()) continue;
        for (Analyzer::Fix& f : Analyzer::QuickFixes(text, d)) {
            bool dup = std::any_of(fixes.begin(), fixes.end(), [&](const Analyzer::Fix& x) { return x.title == f.title; });
            if (!dup) fixes.push_back(std::move(f));
        }
    }
    if (fixes.empty()) {
        ShowMessage(hwnd_, Tr(L"Для этой строки нет быстрых исправлений."), false);
        return;
    }
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < fixes.size(); i++) AppendMenuW(menu, MF_STRING, i + 1, Widen(fixes[i].title).c_str());
    Sci_Position caret = ed->Call(SCI_GETCURRENTPOS);
    POINT pt{static_cast<LONG>(ed->Call(SCI_POINTXFROMPOSITION, 0, caret)),
             static_cast<LONG>(ed->Call(SCI_POINTYFROMPOSITION, 0, caret) + ed->Call(SCI_TEXTHEIGHT, 0))};
    ClientToScreen(ed->hwnd(), &pt);
    int chosen = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
    if (chosen <= 0) return;
    // Apply through Scintilla (bottom up), so breakpoints, folding and undo keep working.
    std::vector<Analyzer::Edit> edits = fixes[chosen - 1].edits;
    std::stable_sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) {
        return a.line != b.line ? a.line > b.line : a.replace > b.replace;
    });
    std::string eol = ed->Call(SCI_GETEOLMODE) == SC_EOL_LF ? "\n" : "\r\n";
    ed->Call(SCI_BEGINUNDOACTION);
    for (const Analyzer::Edit& e : edits) {
        Sci_Position lines = ed->Call(SCI_GETLINECOUNT);
        if (e.replace && e.line < lines) {
            ed->Call(SCI_SETTARGETRANGE, ed->Call(SCI_POSITIONFROMLINE, e.line), ed->Call(SCI_GETLINEENDPOSITION, e.line));
            ed->Call(SCI_REPLACETARGET, e.text.size(), reinterpret_cast<sptr_t>(e.text.c_str()));
        } else {
            Sci_Position at = e.line < lines ? ed->Call(SCI_POSITIONFROMLINE, e.line) : ed->Call(SCI_GETLENGTH);
            std::string insert = e.line < lines ? e.text + eol : eol + e.text;
            ed->Call(SCI_INSERTTEXT, at, reinterpret_cast<sptr_t>(insert.c_str()));
        }
    }
    ed->Call(SCI_ENDUNDOACTION);
    RequestCheck(ed);
}

// Which argument registers (and stack slots) are set before "call name" on a line, for the hint.
std::string MainWindow::CallArgumentsText(Editor* ed, int line) {
    auto args = Analyzer::ArgumentsOfCall(ed->Text(), line);
    if (!args || (!args->known && args->arguments.empty())) return {};
    std::string out = Tr(std::string("Аргументы перед этим call:"));
    auto row = [&](const Analyzer::Argument& a, bool required) {
        out += "\n  " + PadRight(a.where, 9) + PadRight(a.name.empty() ? "…" : a.name, 10);
        if (a.setOnLine >= 0) out += "✓ " + Tr(std::string("строка ")) + std::to_string(a.setOnLine + 1);
        else out += required ? "✗ " + Tr(std::string("не задан")) : "—";
    };
    for (const auto& a : args->arguments) row(a, args->known);
    for (const auto& a : args->extra) row(a, false);
    if (args->known && args->variadic) out += "\n  " + Tr(std::string("(дальше — сколько нужно формату)"));
    return out;
}

// While typing "call name" with a known C / WinAPI function: its prototype and the arguments set so far.
void MainWindow::ShowCallArguments(Editor* ed) {
    Sci_Position pos = ed->Call(SCI_GETCURRENTPOS);
    int line = static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, pos));
    Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, line);
    std::string before(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, lineStart, pos - lineStart)),
                       pos - lineStart);
    static const std::regex call("^\\s*(?:[\\w.]+:\\s*)?call\\s+([A-Za-z_]\\w*)$", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(before, m, call)) return;
    std::string card = Hints::Function(m[1].str());
    if (card.empty()) return;
    std::string tip = card.substr(0, Hints::HeaderLength(card));
    std::string args = CallArgumentsText(ed, line);
    if (!args.empty()) tip += "\n" + args;
    ed->Call(SCI_CALLTIPSHOW, lineStart + m.position(1), reinterpret_cast<sptr_t>(tip.c_str()));
    ed->Call(SCI_CALLTIPSETHLT, 0, Hints::HeaderLength(card));
}

// Ctrl+Alt+Up/Down: a caret on the line above / below the main one, at the same column.
void MainWindow::AddCaret(Editor* ed, bool down) {
    Sci_Position caret = ed->Call(SCI_GETSELECTIONNCARET, ed->Call(SCI_GETMAINSELECTION));
    Sci_Position line = ed->Call(SCI_LINEFROMPOSITION, caret) + (down ? 1 : -1);
    if (line < 0 || line >= ed->Call(SCI_GETLINECOUNT)) return;
    Sci_Position column = ed->Call(SCI_GETCOLUMN, caret);
    Sci_Position pos = ed->Call(SCI_FINDCOLUMN, line, column);
    ed->Call(SCI_ADDSELECTION, pos, pos);
    ed->Call(SCI_SCROLLCARET);
}

// Uses of the word under the caret are boxed in the whole file (a short pause after the caret stops).
void MainWindow::HighlightOccurrences(Editor* ed) {
    Sci_Position len = ed->Call(SCI_GETLENGTH);
    ed->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorOccurrence);
    ed->Call(SCI_INDICATORCLEARRANGE, 0, len);
    if (ed->Call(SCI_GETSELECTIONS) > 1 || len > 2'000'000) return;
    Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
    Sci_Position pos = ed->Call(SCI_GETCURRENTPOS);
    Sci_Position ws, we;
    if (!ed->WordRange(pos, ws, we) && !(pos > 0 && ed->WordRange(pos - 1, ws, we))) return;
    if (e > s && (s != ws || e != we)) return;  // a selection that is not the word
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, ws));
    if (style == T_COMMENT || style == T_STRING || style == T_NUMBER) return;
    std::string word(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, ws, we - ws)), we - ws);
    while (!word.empty() && word.back() == '.') word.pop_back();
    if (word.empty()) return;
    std::string text = ed->Text();
    std::vector<Completion::Occurrence> hits;
    if (style == T_REGISTER || style == T_INSTRUCTION || style == T_DIRECTIVE || style == T_KEYWORD) {
        // Built-in words: every token with the same text, ignoring case.
        std::vector<Token> tokens;
        Sci_Position lines = ed->Call(SCI_GETLINECOUNT);
        for (Sci_Position l = 0; l < lines; l++) {
            std::string lt = ed->LineText(l);
            Tokenize(lt.data(), static_cast<int>(lt.size()), tokens);
            for (const Token& t : tokens) {
                if (t.type == style && t.end - t.start == static_cast<int>(word.size()) &&
                    _strnicmp(lt.c_str() + t.start, word.c_str(), word.size()) == 0) {
                    hits.push_back({static_cast<int>(l) + 1, t.start, t.end - t.start, false, false, {}});
                }
            }
        }
    } else {
        std::string sym, scope;
        if (!SymbolUnderCaret(ed, ws, sym, scope, true)) return;
        hits = Completion::FindOccurrences(text, sym, scope);
    }
    if (hits.size() < 2) return;
    for (const auto& h : hits) {
        Sci_Position at = ed->Call(SCI_POSITIONFROMLINE, h.line - 1) + h.column;
        ed->Call(SCI_INDICATORFILLRANGE, at, h.length);
    }
    InvalidateRect(ed == SplitEditor() ? markBar2_ : markBar_, nullptr, FALSE);
}

// Brackets next to the caret and the pairs %if/%endif, %macro/%endmacro, %rep/%endrep.
void MainWindow::UpdateBracePairs(Editor* ed) {
    Sci_Position len = ed->Call(SCI_GETLENGTH);
    ed->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorPair);
    ed->Call(SCI_INDICATORCLEARRANGE, 0, len);
    Sci_Position pos = ed->Call(SCI_GETCURRENTPOS);
    auto isBrace = [&](Sci_Position p) {
        int c = static_cast<int>(ed->Call(SCI_GETCHARAT, p));
        int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, p));
        return (c == '[' || c == ']' || c == '(' || c == ')') && style != T_COMMENT && style != T_STRING;
    };
    Sci_Position at = isBrace(pos) ? pos : (pos > 0 && isBrace(pos - 1) ? pos - 1 : -1);
    if (at >= 0) {
        Sci_Position match = ed->Call(SCI_BRACEMATCH, at, 0);
        if (match >= 0) ed->Call(SCI_BRACEHIGHLIGHT, at, match);
        else ed->Call(SCI_BRACEBADLIGHT, at);
    } else {
        ed->Call(SCI_BRACEHIGHLIGHT, INVALID_POSITION, INVALID_POSITION);
    }

    // Directive pairs: find the directive on the caret line, then walk with a depth counter.
    Sci_Position line = ed->Call(SCI_LINEFROMPOSITION, pos);
    auto directive = [&](Sci_Position l, Sci_Position& start) -> std::string {
        std::string t = ed->LineText(l);
        size_t i = t.find_first_not_of(" \t");
        if (i == std::string::npos || t[i] != '%') return {};
        size_t j = i + 1;
        while (j < t.size() && isalpha(static_cast<unsigned char>(t[j]))) j++;
        start = ed->Call(SCI_POSITIONFROMLINE, l) + static_cast<Sci_Position>(i);
        std::string w = t.substr(i, j - i);
        for (char& c : w) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return w;
    };
    auto kind = [](const std::string& w) {  // 1 open, -1 close, 2 middle (%else) — per family
        if (w == "%macro" || w == "%imacro" || w == "%rep" || w.rfind("%if", 0) == 0) return 1;
        if (w == "%endmacro" || w == "%endrep" || w == "%endif") return -1;
        if (w == "%else" || w.rfind("%elif", 0) == 0) return 2;
        return 0;
    };
    auto family = [](const std::string& w) {
        if (w.find("macro") != std::string::npos) return 1;
        if (w.find("rep") != std::string::npos) return 2;
        return 3;  // %if / %elif / %else / %endif
    };
    Sci_Position start = 0;
    std::string w = directive(line, start);
    int k = kind(w);
    if (!k) return;
    std::vector<std::pair<Sci_Position, size_t>> marks{{start, w.size()}};
    int fam = family(w);
    Sci_Position lines = ed->Call(SCI_GETLINECOUNT);
    auto walk = [&](int dir) {
        int depth = 0;
        for (Sci_Position l = line + dir; l >= 0 && l < lines; l += dir) {
            Sci_Position s2 = 0;
            std::string w2 = directive(l, s2);
            int k2 = kind(w2);
            if (!k2 || family(w2) != fam) continue;
            if ((dir == 1 && k2 == 1) || (dir == -1 && k2 == -1)) {
                depth++;
                continue;
            }
            if ((dir == 1 && k2 == -1) || (dir == -1 && k2 == 1)) {
                if (depth == 0) {
                    marks.push_back({s2, w2.size()});
                    return;
                }
                depth--;
                continue;
            }
            if (k2 == 2 && depth == 0) marks.push_back({s2, w2.size()});
        }
    };
    if (k == 1 || k == 2) walk(1);
    if (k == -1 || k == 2) walk(-1);
    if (marks.size() < 2) return;
    for (auto [s2, n] : marks) ed->Call(SCI_INDICATORFILLRANGE, s2, n);
}

void MainWindow::FoldAll(Editor* ed, bool fold) {
    ed->Call(SCI_COLOURISE, 0, -1);  // fold levels come from the lexer
    ed->Call(SCI_FOLDALL, fold ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
}

// ---- find in files ------------------------------------------------------------------------------------------------

// Ctrl+Shift+F: every match in the project's text files goes to the Search tab; "replace" edits the files in
// tabs (unsaved, so it can be reviewed and undone).
void MainWindow::FindInFiles() {
    FindInFilesRequest initial = lastFindInFiles_;
    if (Editor* ed = Active()) {
        Sci_Position s = ed->Call(SCI_GETSELECTIONSTART), e = ed->Call(SCI_GETSELECTIONEND);
        if (e > s && e - s < 200) {
            std::string sel(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, s, e - s)), e - s);
            if (sel.find('\n') == std::string::npos) initial.what = Widen(sel);
        } else {
            std::string w = ed->WordAt(ed->Call(SCI_GETCURRENTPOS));
            if (!w.empty() && initial.what.empty()) initial.what = Widen(w);
        }
    }
    auto request = ShowFindInFilesDialog(hwnd_, initial);
    if (!request || request->what.empty()) return;
    lastFindInFiles_ = *request;
    const FindInFilesRequest& r = *request;

    // Sources: open tabs (their current text) and the project's text files from disk.
    std::vector<UsageSource> sources;
    std::set<std::wstring> seen;
    for (const auto& e : editors_) {
        if (!e->file().empty()) seen.insert(Debugger::Key(e->file()));
        sources.push_back({e->file(), e.get(), e->Text()});
    }
    static const std::set<std::wstring> kText = {L".asm", L".inc", L".mac", L".s", L".nasm", L".txt", L".md", L".bat",
                                                 L".c", L".h", L".cpp", L".ini", L".def"};
    for (const fs::path& p : ProjectFiles()) {
        if (!kText.count(Lower(p.extension().wstring())) || !seen.insert(Debugger::Key(p)).second) continue;
        std::error_code ec;
        if (fs::file_size(p, ec) > 4'000'000 || ec) continue;
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        sources.push_back({p, nullptr, ss.str()});
    }

    std::regex re;
    std::string needle = Narrow(r.what);
    try {
        auto flags = std::regex::ECMAScript | (r.matchCase ? std::regex::flag_type{} : std::regex::icase);
        std::string pattern = r.regex ? needle : std::regex_replace(needle, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
        if (r.wholeWord) pattern = "\\b(?:" + pattern + ")\\b";
        re = std::regex(pattern, flags);
    } catch (const std::regex_error&) {
        ShowMessage(hwnd_, Tr(L"Неверное регулярное выражение."));
        return;
    }
    struct Match {
        int line, column, length;
    };
    ClearPane(Pane::Search);
    ShowPane(Pane::Search);
    size_t total = 0, files = 0;
    std::vector<std::pair<const UsageSource*, std::vector<Match>>> found;
    for (const UsageSource& src : sources) {
        std::vector<Match> matches;
        int lineNo = 0;
        size_t pos = 0;
        while (pos <= src.text.size()) {
            size_t end = src.text.find('\n', pos);
            if (end == std::string::npos) end = src.text.size();
            std::string line = src.text.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lineNo++;
            for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator(); ++it) {
                if (it->length(0) == 0) break;
                matches.push_back({lineNo, static_cast<int>(it->position(0)), static_cast<int>(it->length(0))});
            }
            pos = end + 1;
        }
        if (matches.empty()) continue;
        files++;
        total += matches.size();
        std::wstring name = src.file.empty() ? src.editor->Title()
                          : (!projectRoot_.empty() && IsUnder(src.file, projectRoot_)
                                 ? NormalizePath(src.file).lexically_relative(NormalizePath(projectRoot_)).wstring()
                                 : src.file.wstring());
        int lastLine = 0;
        for (const Match& m : matches) {
            if (m.line == lastLine) continue;  // one line in the list per line of text
            lastLine = m.line;
            size_t ls = 0;
            for (int l = 1; l < m.line; l++) ls = src.text.find('\n', ls) + 1;
            std::string code = src.text.substr(ls, src.text.find('\n', ls) - ls);
            if (!code.empty() && code.back() == '\r') code.pop_back();
            size_t first = code.find_first_not_of(" \t");
            code = first == std::string::npos ? std::string() : code.substr(first);
            PrintTo(Pane::Search, Narrow(name) + ":" + std::to_string(m.line) + ": ", Out::Input);
            PrintTo(Pane::Search, code + "\n", Out::Normal);
        }
        found.push_back({&src, std::move(matches)});
    }
    std::string header = (r.replace ? Tr(std::string("Заменено")) : Tr(std::string("Найдено"))) + " «" + needle + "»: " +
                         std::to_string(total) + " " + Tr(std::string("в файлах:")) + " " + std::to_string(files);
    if (!r.replace) {
        PrintTo(Pane::Search, "\n" + header + " — " + Tr(std::string("двойной клик открывает место")) + "\n", Out::Info);
        // Mark the hits in open tabs.
        for (const auto& [src, matches] : found) {
            if (!src->editor) continue;
            src->editor->Call(SCI_SETINDICATORCURRENT, Editor::kIndicatorSearch);
            src->editor->Call(SCI_INDICATORCLEARRANGE, 0, src->editor->Call(SCI_GETLENGTH));
            for (const Match& m : matches) {
                Sci_Position at = src->editor->Call(SCI_POSITIONFROMLINE, m.line - 1) + m.column;
                src->editor->Call(SCI_INDICATORFILLRANGE, at, m.length);
            }
        }
        return;
    }
    // Replace: open each file and edit it from the end, so earlier offsets stay valid.
    std::string with = Narrow(r.replaceWith);
    for (const auto& [src, matches] : found) {
        Editor* target = src->editor ? src->editor : OpenFile(src->file);
        if (!target) continue;
        target->Call(SCI_BEGINUNDOACTION);
        for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
            Sci_Position at = target->Call(SCI_POSITIONFROMLINE, it->line - 1) + it->column;
            std::string original(reinterpret_cast<const char*>(target->Call(SCI_GETRANGEPOINTER, at, it->length)), it->length);
            std::string replacement = r.regex ? std::regex_replace(original, re, with, std::regex_constants::format_first_only) : with;
            target->Call(SCI_SETTARGETRANGE, at, at + it->length);
            target->Call(SCI_REPLACETARGET, replacement.size(), reinterpret_cast<sptr_t>(replacement.c_str()));
        }
        target->Call(SCI_ENDUNDOACTION);
    }
    InvalidateChrome();
    PrintTo(Pane::Search, "\n" + header + ". " + Tr(std::string("Файлы не сохранены: Ctrl+Z — отменить, «Сохранить всё» — сохранить.")) +
                              "\n", Out::Success);
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
        ShowMessage(findDialog_ ? findDialog_ : hwnd_, Tr(L"Заменено: ") + std::to_wstring(count), false);
    }
}
