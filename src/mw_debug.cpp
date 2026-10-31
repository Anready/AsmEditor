#include "mw_internal.h"
#include "hints.h"

namespace {

// Collects styled text for a panel view and remembers which line each piece starts on.
struct PanelText {
    std::vector<std::pair<std::string, int>> runs;
    int line = 0;
    void Add(const std::string& text, int style) {
        runs.push_back({text, style});
        line += static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    }
};

std::string HexN(uint64_t v, int digits) {
    char buf[24];
    sprintf_s(buf, "%0*llX", digits, static_cast<unsigned long long>(v));
    return buf;
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
}

// ---- debugging --------------------------------------------------------------------------------------------------

void MainWindow::DebugCommand(int id) {
    if (!debugging_) {
        if (id == ID_DEBUG) StartBuild(After::Debug);
        else if (id == ID_STEP_OVER || id == ID_STEP_INTO) StartBuild(After::DebugEntry);
        return;
    }
    if (id == ID_PAUSE) {
        debugger_->Pause();
        return;
    }
    if (!debugger_->Stopped()) return;
    ClearExecutionLine();
    debugStatus_ = Tr(L"выполняется…");
    debugStatusColor_ = Theme::Success;
    switch (id) {
    case ID_DEBUG: debugger_->Continue(); break;
    case ID_STEP_OVER: debugger_->StepOver(); break;
    case ID_STEP_INTO: debugger_->StepInto(); break;
    case ID_STEP_OUT: debugger_->StepOut(); break;
    }
    // Stopped() turns false on the debugger thread; repaint once it has picked up the command.
    InvalidateChrome();
    SetTimer(hwnd_, kRepaintTimer, 50, [](HWND h, UINT, UINT_PTR id, DWORD) {
        KillTimer(h, id);
        InvalidateRect(h, nullptr, FALSE);
    });
    // If the program keeps running (e.g. waits in scanf), typing should go to it, not into the source.
    SetTimer(hwnd_, kConsoleFocusTimer, 400, nullptr);
}

void MainWindow::SetDebugging(bool on) {
    debugging_ = on;
    haveLastContext_ = false;
    prevMem_.clear();
    frames_.clear();
    if (on) {
        debugStatus_ = Tr(L"выполняется…");
        debugStatusColor_ = Theme::Success;
        RenderPanel(*regsView_, {{Tr(std::string("Программа выполняется.\nПоставьте точку останова (F9 или клик слева от "
                                                 "номера строки) или нажмите «Пауза».\n")), P_DIM}});
        RenderPanel(*memView_, {});
        RenderPanel(*stackView_, {});
        RenderPanel(*disasmView_, {});
        RenderWatches();
    } else {
        debugStatus_.clear();
        RenderWatches();
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnDebugStopped(std::unique_ptr<DebugStop> stop) {
    if (!debugging_) return;
    switch (stop->reason) {
    case DebugStop::Reason::Breakpoint: debugStatus_ = Tr(L"точка останова"); break;
    case DebugStop::Reason::Step: debugStatus_ = Tr(L"шаг"); break;
    case DebugStop::Reason::Entry: debugStatus_ = Tr(L"начало main"); break;
    case DebugStop::Reason::Pause: debugStatus_ = Tr(L"пауза"); break;
    case DebugStop::Reason::Int3: debugStatus_ = Tr(L"int3 в программе"); break;
    case DebugStop::Reason::Exception: debugStatus_ = Tr(L"исключение"); break;
    case DebugStop::Reason::Watchpoint: debugStatus_ = Tr(L"изменились данные"); break;
    case DebugStop::Reason::RunTo: debugStatus_ = Tr(L"до курсора"); break;
    case DebugStop::Reason::StepBack: debugStatus_ = Tr(L"шаг назад"); break;
    }
    debugStatusColor_ = stop->reason == DebugStop::Reason::Exception ? Theme::Error : Theme::Warning;

    std::wstring where = stop->location.valid()
                             ? stop->location.file.filename().wstring() + L":" + std::to_wstring(stop->location.line)
                             : debugger_->SymbolAt(stop->context.Rip);
    if (where.empty()) {
        wchar_t buf[32];
        swprintf_s(buf, L"0x%llX", static_cast<unsigned long long>(stop->context.Rip));
        where = buf;
    }
    if (stop->reason == DebugStop::Reason::Exception) {
        Print(Narrow(stop->message + L"\n    " + Tr(L"в ") + where) + "\n", Out::Error);
    } else if (stop->reason == DebugStop::Reason::Watchpoint) {
        Print(Narrow(stop->message + L"  (" + where + L")") + "\n", Out::Warning);
    } else if (!stop->location.valid()) {
        Print(Tr(std::string("Остановлено вне исходного кода: ")) + Narrow(where) + "\n", Out::Info);
    }
    if (!stop->message.empty() && stop->reason == DebugStop::Reason::Breakpoint) {
        PrintTo(Pane::DebugOutput, Narrow(where + L": " + stop->message) + "\n", Out::Info);
    }

    ClearExecutionLine();
    if (stop->location.valid()) {
        if (Editor* ed = OpenFile(stop->location.file, stop->location.line)) {
            ed->SetExecutionLine(stop->location.line);
            execFile_ = stop->location.file;
            // What the line is about to do, with the current values.
            auto eval = [&](const std::string& expr) {
                std::wstring error;
                return debugger_->Evaluate(expr, error);
            };
            std::string note = Analyzer::ExplainLine(ed->LineText(stop->location.line - 1), eval, stop->context.EFlags);
            if (!note.empty()) ed->SetLineNotes({{stop->location.line, " ▸ " + note + " "}});
        }
    }
    frames_ = debugger_->CallStack();
    RenderDebugPanel();
    lastContext_ = stop->context;
    haveLastContext_ = true;
    if (GetForegroundWindow() != hwnd_) SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRect(markBar_, nullptr, FALSE);
    InvalidateRect(markBar2_, nullptr, FALSE);
}

void MainWindow::OnDebugExited(DWORD code, const std::string& stats) {
    if (!debugging_) return;
    debugger_.reset();
    if (job_ && !process_) {  // the terminal holder of a WSL debugging session
        TerminateJobObject(job_, 0);
        CloseHandle(job_);
        job_ = nullptr;
    }
    ClearExecutionLine();
    SetDebugging(false);
    exitStats_ = stats;
    FinishConsole(code);
}

void MainWindow::ClearExecutionLine() {
    for (auto& e : editors_) {
        e->SetExecutionLine(0);
        if (profile_.empty()) e->SetLineNotes({});  // the "explain the line" note
        else ShowProfileNotes(e.get());
    }
    execFile_.clear();
}

void MainWindow::ToggleBreakpoint(Editor* ed, int line) {
    if (!ed || line <= 0) return;
    ed->ToggleBreakpoint(line);
    if (debugger_ && !ed->file().empty()) debugger_->SetBreakpoints(ed->file(), ed->Breakpoints());
    InvalidateRect(ed == SplitEditor() ? markBar2_ : markBar_, nullptr, FALSE);
}

// Condition of the breakpoint on a line: an expression ("rcx == 5"), a hit number ("#10") or both; "log: ..."
// turns it into a logpoint that prints to "Debug output" instead of stopping.
void MainWindow::EditBreakpointCondition(Editor* ed, int line, bool logpoint) {
    if (!ed || line <= 0) return;
    std::string current;
    auto bps = ed->Breakpoints();
    if (bps.count(line)) current = bps[line];
    if (logpoint && !Editor::IsLogpoint(current)) current += current.empty() ? "log: " : " log: ";
    auto text = InputBox(hwnd_, logpoint ? Tr(L"Точка останова с выводом") : Tr(L"Условие точки останова"),
                         logpoint ? Tr(L"Печатать без остановки (log: rcx = {rcx}, {[counter]:x} — в hex; "
                                       L"перед log: можно условие):")
                                  : Tr(L"Останавливаться, когда (rcx == 5, [counter] > 3, #10 — на 10-й раз; пусто — всегда):"),
                         Widen(current));
    if (!text) return;
    ed->SetBreakpointCondition(line, Narrow(Trim(*text)));
    if (debugger_ && !ed->file().empty()) debugger_->SetBreakpoints(ed->file(), ed->Breakpoints());
    SaveProjectSession();
    InvalidateRect(ed == SplitEditor() ? markBar2_ : markBar_, nullptr, FALSE);
}

// Right click on the breakpoint margin.
void MainWindow::BreakpointMenu(Editor* ed, int line) {
    bool has = ed->HasBreakpoint(line);
    HMENU menu = CreatePopupMenu();
    enum { kToggle = 1, kCondition, kLogpoint, kRunTo, kSetNext, kClearAll };
    AppendMenuW(menu, MF_STRING, kToggle, has ? Tr(L"Убрать точку останова") : Tr(L"Поставить точку останова"));
    AppendMenuW(menu, MF_STRING, kCondition, has ? Tr(L"Изменить условие…") : Tr(L"Точка останова с условием…"));
    AppendMenuW(menu, MF_STRING, kLogpoint, Tr(L"Точка останова с выводом (без остановки)…"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kRunTo, MenuText(ID_RUN_TO_CURSOR).c_str());
    AppendMenuW(menu, MF_STRING | (DebugStopped() ? 0 : MF_GRAYED), kSetNext, MenuText(ID_SET_NEXT).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kClearAll, Tr(L"Удалить все точки останова"));
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
    auto caretToLine = [&] { ed->Call(SCI_GOTOLINE, line - 1); };
    switch (cmd) {
    case kToggle: ToggleBreakpoint(ed, line); break;
    case kCondition:
        if (!has) ed->ToggleBreakpoint(line);
        EditBreakpointCondition(ed, line);
        break;
    case kLogpoint:
        if (!has) ed->ToggleBreakpoint(line);
        EditBreakpointCondition(ed, line, true);
        break;
    case kRunTo:
        caretToLine();
        RunToCursor();
        break;
    case kSetNext:
        caretToLine();
        SetNextStatement();
        break;
    case kClearAll: OnCommand(ID_CLEAR_BPS); break;
    }
}

void MainWindow::StashBreakpoints(Editor* ed) {
    if (!ed || ed->file().empty()) return;
    auto lines = ed->Breakpoints();
    if (lines.empty()) breakpoints_.erase(Debugger::Key(ed->file()));
    else breakpoints_[Debugger::Key(ed->file())] = lines;
}

Debugger::Breakpoints MainWindow::CollectBreakpoints() const {
    Debugger::Breakpoints all = breakpoints_;
    for (const auto& e : editors_) {
        if (e->file().empty()) continue;
        auto lines = e->Breakpoints();
        if (lines.empty()) all.erase(Debugger::Key(e->file()));
        else all[Debugger::Key(e->file())] = lines;
    }
    return all;
}

// Ctrl+F10: continue to the caret line (or start debugging and stop there).
void MainWindow::RunToCursor() {
    Editor* ed = focusRight_ && SplitEditor() ? SplitEditor() : Active();
    if (!ed) return;
    if (ed->file().empty() && !SaveAs(ed)) return;
    SourceLocation where{ed->file(), ed->CaretLine()};
    if (!debugging_) {
        runTo_ = where;
        StartBuild(After::RunToCursor);
        return;
    }
    if (!DebugStopped()) return;
    if (!debugger_->RunTo(where)) {
        ShowMessage(hwnd_, Tr(L"На этой строке (и в нескольких следующих) нет кода."), false);
        return;
    }
    ClearExecutionLine();
    debugStatus_ = Tr(L"выполняется…");
    debugStatusColor_ = Theme::Success;
    InvalidateChrome();
    SetTimer(hwnd_, kConsoleFocusTimer, 400, nullptr);
}

// Ctrl+Shift+F10: the next instruction to run is the first one of the caret line (rip changes).
void MainWindow::SetNextStatement() {
    Editor* ed = focusRight_ && SplitEditor() ? SplitEditor() : Active();
    if (!ed || !DebugStopped()) return;
    std::wstring error;
    SourceLocation where{ed->file(), ed->CaretLine()};
    if (!debugger_->SetNextStatement(where, error)) {
        ShowMessage(hwnd_, std::wstring(Tr(L"Нельзя перенести выполнение: ")) + error, false);
        return;
    }
    SourceLocation actual = debugger_->LocationOf(debugger_->Context().Rip);
    ClearExecutionLine();
    if (actual.valid()) ed->SetExecutionLine(actual.line);
    debugStatus_ = Tr(L"выполнение перенесено");
    PrintTo(Pane::DebugOutput, Tr(std::string("rip перенесён на строку ")) + std::to_string(actual.line) + "\n", Out::Info);
    RenderDebugPanel();
}

// Ctrl+Shift+F11: back to the previous stop (registers, global variables and the stack are restored).
void MainWindow::StepBack() {
    if (!DebugStopped()) return;
    std::wstring error;
    if (!debugger_->StepBack(error)) {
        ShowMessage(hwnd_, std::wstring(Tr(L"Шаг назад невозможен: ")) + Tr(error.c_str()), false);
        return;
    }
    static bool explained = false;
    if (!explained) {
        explained = true;
        PrintTo(Pane::DebugOutput, Tr(std::string("Шаг назад восстанавливает регистры, глобальные переменные и стек. "
                                                  "Вывод в консоль, файлы и память из кучи не откатываются.\n")),
                Out::Info);
    }
}

// The flags and what every condition code gives with them; on a conditional instruction, whether it will jump.
void MainWindow::RenderFlags() {
    flagLines_.clear();
    PanelText r;
    auto mark = [&](int bit) {
        if (static_cast<int>(flagLines_.size()) <= r.line) flagLines_.resize(r.line + 1, -1);
        flagLines_[r.line] = bit;
    };
    if (!DebugStopped()) {
        RenderPanel(*flagsView_, {{Tr(std::string("Флаги видны, когда программа остановлена.\n")), P_DIM}});
        return;
    }
    const CONTEXT& c = debugger_->Context();
    uint32_t f = c.EFlags;
    const CONTEXT* prev = haveLastContext_ ? &lastContext_ : nullptr;

    // The instruction about to run.
    uint8_t code[16]{};
    size_t got = debugger_->ReadMemory(c.Rip, code, sizeof(code));
    if (auto ins = Conditions::Decode(code, got, f, c.Rcx, debugger_->Is32())) {
        using Kind = Conditions::Instruction::Kind;
        const Conditions::Condition& cond = ins->condition;
        r.Add(Tr(std::string("СЛЕДУЮЩАЯ ИНСТРУКЦИЯ")) + "\n", P_HEADER);
        r.Add(ins->mnemonic, P_CODE);
        if (ins->kind != Kind::Unconditional) {
            r.Add("   " + cond.formula + "   (" + cond.values + ")", P_TEXT);
        }
        r.Add("\n", P_TEXT);
        std::string verdict;
        switch (ins->kind) {
        case Kind::Jump:
        case Kind::Loop:
        case Kind::Unconditional:
            verdict = cond.holds ? Tr(std::string("→ переход будет")) : Tr(std::string("→ перехода не будет, выполнится следующая строка"));
            break;
        case Kind::Move:
            verdict = cond.holds ? Tr(std::string("→ значение будет скопировано")) : Tr(std::string("→ регистр не изменится"));
            break;
        case Kind::Set:
            verdict = cond.holds ? Tr(std::string("→ в байт запишется 1")) : Tr(std::string("→ в байт запишется 0"));
            break;
        }
        r.Add(verdict + "   ", cond.holds ? P_YES : P_NO);
        r.Add(Tr(cond.meaning) + "\n\n", P_DIM);
    }

    r.Add(Tr(std::string("ФЛАГИ")) + "  " + Tr(std::string("(двойной клик — переключить)")) + "\n", P_HEADER);
    static const struct {
        const char* name;
        int bit;
        const char* meaning;
    } flags[] = {
        {"CF", 0, "перенос: беззнаковый результат не поместился (или был заём при вычитании)"},
        {"PF", 2, "чётность: в младшем байте результата чётное число единиц"},
        {"AF", 4, "вспомогательный перенос из бита 3 (для BCD-арифметики)"},
        {"ZF", 6, "ноль: результат равен 0 (после cmp — операнды равны)"},
        {"SF", 7, "знак: старший бит результата (результат отрицательный)"},
        {"DF", 10, "направление строковых инструкций: 1 — к меньшим адресам"},
        {"OF", 11, "переполнение: знаковый результат не поместился"},
    };
    for (const auto& fl : flags) {
        bool on = (f >> fl.bit) & 1;
        bool changed = prev && (((prev->EFlags >> fl.bit) & 1) != on);
        mark(fl.bit);
        r.Add(std::string(fl.name) + (on ? " = 1  " : " = 0  "), changed ? P_CHANGED : on ? P_FLAG_ON : P_DIM);
        r.Add(Tr(std::string(fl.meaning)) + "\n", P_DIM);
    }
    r.Add("\n" + Tr(std::string("УСЛОВИЯ (jcc, cmovcc, setcc)")) + "\n", P_HEADER);
    for (const Conditions::Condition& cond : Conditions::All(f)) {
        std::string names = "j" + cond.name;
        std::istringstream aliases(cond.aliases);
        for (std::string alias; aliases >> alias;) names += "/j" + alias;
        r.Add(cond.holds ? "✓ " : "✗ ", cond.holds ? P_YES : P_NO);
        r.Add(PadRight(names, 12), P_CODE);
        r.Add(PadRight(cond.formula, 20), P_TEXT);
        r.Add(Tr(cond.meaning) + "\n", P_DIM);
    }
    r.Add("\n" + Tr(std::string("Флаги выставляет последняя арифметическая или логическая инструкция (cmp, test, add, sub, "
                                "and…); mov, lea, push, pop и переходы их не меняют.\n")),
          P_DIM);
    RenderPanel(*flagsView_, r.runs);
}

// ---- profiler ---------------------------------------------------------------------------------------------------

void MainWindow::OnProfile(std::vector<ProfileLine> lines) {
    profile_ = std::move(lines);
    for (auto& e : editors_) ShowProfileNotes(e.get());
    uint64_t total = 0, samples = 0;
    for (const ProfileLine& p : profile_) total += p.instructions, samples += p.samples;
    std::vector<const ProfileLine*> byTime;
    for (const ProfileLine& p : profile_) byTime.push_back(&p);
    std::sort(byTime.begin(), byTime.end(), [](auto* a, auto* b) { return a->seconds > b->seconds; });
    std::string text = "\n" + (samples ? Tr(std::string("Профиль по таймеру: замеров — ")) + std::to_string(samples)
                                        : Tr(std::string("Профиль: выполнено инструкций — ")) + std::to_string(total)) +
                       ". " + Tr(std::string("Дольше всего:")) + "\n";
    for (size_t i = 0; i < byTime.size() && i < 8; i++) {
        const ProfileLine& p = *byTime[i];
        char buf[160];
        if (samples) {
            sprintf_s(buf, "  %s:%d  %.1f %s, %.0f%%\n", Narrow(p.location.file.filename().wstring()).c_str(),
                      p.location.line, p.seconds * 1000, Tr(std::string("мс")).c_str(), 100.0 * p.samples / samples);
        } else {
            sprintf_s(buf, "  %s:%d  × %llu, %llu %s, %.3f %s\n", Narrow(p.location.file.filename().wstring()).c_str(),
                      p.location.line, static_cast<unsigned long long>(p.hits),
                      static_cast<unsigned long long>(p.instructions), Tr(std::string("инстр.")).c_str(), p.seconds * 1000,
                      Tr(std::string("мс")).c_str());
        }
        text += buf;
    }
    text += Tr(std::string("Числа у строк в редакторе: сколько раз выполнилась строка, сколько инструкций и время "
                           "(с вызовами printf и т. п.; само профилирование замедляет программу). Убрать — "
                           "Запуск → «Убрать результаты профилирования».\n"));
    Print(text, Out::Info);
}

void MainWindow::ShowProfileNotes(Editor* ed) {
    if (!ed || ed->file().empty()) return;
    std::map<int, std::string> notes;
    uint64_t samples = 0;
    for (const ProfileLine& p : profile_) samples += p.samples;
    for (const ProfileLine& p : profile_) {
        if (!SamePath(p.location.file, ed->file())) continue;
        char buf[128];
        if (samples) {
            if (!p.samples) continue;
            sprintf_s(buf, " ≈ %.1f %s · %.0f%% ", p.seconds * 1000, Tr(std::string("мс")).c_str(), 100.0 * p.samples / samples);
            notes[p.location.line] = buf;
            continue;
        }
        sprintf_s(buf, " × %llu · %llu %s · %.3f %s ", static_cast<unsigned long long>(p.hits),
                  static_cast<unsigned long long>(p.instructions), Tr(std::string("инстр.")).c_str(), p.seconds * 1000,
                  Tr(std::string("мс")).c_str());
        notes[p.location.line] = buf;
    }
    ed->SetLineNotes(notes);
}

void MainWindow::ClearProfile() {
    if (profile_.empty()) return;
    profile_.clear();
    for (auto& e : editors_) e->SetLineNotes({});
}

void MainWindow::RenderPanel(Editor& v, const Runs& runs) {
    Sci_Position first = v.Call(SCI_GETFIRSTVISIBLELINE);
    v.Call(SCI_SETREADONLY, 0);
    v.Call(SCI_CLEARALL);
    for (const auto& [text, style] : runs) {
        Sci_Position pos = v.Call(SCI_GETLENGTH);
        v.Call(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.data()));
        v.Call(SCI_STARTSTYLING, pos);
        v.Call(SCI_SETSTYLING, text.size(), style);
    }
    v.Call(SCI_SETREADONLY, 1);
    v.Call(SCI_SETFIRSTVISIBLELINE, first);
}

void MainWindow::RenderDebugPanel() {
    RenderRegisters();
    RenderCallStack();
    RenderWatches();
    if (debugTab_ == DebugTab::Disasm) RenderDisassembly();
    RenderFlags();
    RenderMemory();
}

void MainWindow::ShowDebugTab(DebugTab tab) {
    debugTab_ = tab;
    Layout();
    if (tab == DebugTab::Disasm) RenderDisassembly();
    if (tab == DebugTab::Watch) {
        RenderWatches();
        SetFocus(watchExpr_);
    }
}

void MainWindow::RenderRegisters() {
    if (!DebugStopped()) return;
    const CONTEXT& c = debugger_->Context();
    const CONTEXT* prev = haveLastContext_ ? &lastContext_ : nullptr;
    bool is32 = debugger_->Is32();
    int digits = is32 ? 8 : 16;
    PanelText r;
    regLines_.clear();
    auto lineFor = [&](const std::string& name) {
        if (static_cast<int>(regLines_.size()) <= r.line) regLines_.resize(r.line + 1);
        regLines_[r.line] = name;
    };
    auto annotate = [&](uint64_t v) {
        std::wstring sym = debugger_->SymbolAt(v);
        if (!sym.empty()) r.Add("  " + Narrow(sym), P_SYMBOL);
        else if (v <= 0xFFFFF) r.Add("  " + std::to_string(v), P_DIM);
        else if (static_cast<int64_t>(v) < 0 && static_cast<int64_t>(v) > -0x100000)
            r.Add("  " + std::to_string(static_cast<int64_t>(v)), P_DIM);
    };

    r.Add(Tr(std::string("РЕГИСТРЫ")) + "  " + Tr(std::string("(двойной клик — изменить)")) + "\n", P_HEADER);
    static const char* names64[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rip"};
    static const char* names32[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip"};
    std::vector<const char*> names = is32 ? std::vector<const char*>(std::begin(names32), std::end(names32))
                                          : std::vector<const char*>(std::begin(names64), std::end(names64));
    for (const char* n : names) {
        uint64_t v = *Debugger::Register(c, n);
        bool changed = prev && *Debugger::Register(*prev, n) != v;
        lineFor(n);
        r.Add(PadRight(n, 4), P_LABEL);
        r.Add(HexN(v, digits), changed ? P_CHANGED : P_TEXT);
        annotate(v);
        r.Add("\n", P_TEXT);
    }
    lineFor("flags");
    r.Add("flg ", P_LABEL);
    static const std::pair<const char*, int> flags[] = {{"CF", 0}, {"PF", 2}, {"AF", 4}, {"ZF", 6}, {"SF", 7},
                                                        {"TF", 8}, {"IF", 9}, {"DF", 10}, {"OF", 11}};
    for (auto [name, bit] : flags) {
        bool on = (c.EFlags >> bit) & 1;
        bool changed = prev && (((prev->EFlags >> bit) & 1) != on);
        r.Add(std::string(name) + (on ? "=1 " : "=0 "), changed ? P_CHANGED : on ? P_FLAG_ON : P_DIM);
    }

    // The stack with what is known about it: return addresses of the call stack (frame boundaries) and rbp.
    // Slots that changed since the previous stop flash and fade out; a line says what happened to rsp.
    r.Add("\n\n" + Tr(std::string("СТЕК")) + "\n", P_HEADER);
    int ptr = debugger_->PointerSize();
    uint64_t stack[24]{};
    std::vector<uint8_t> raw(24 * ptr);
    size_t got = debugger_->ReadMemory(c.Rsp, raw.data(), raw.size()) / ptr;
    raw.resize(got * ptr);
    bool fresh = prev && prevStackRsp_ == prev->Rsp && !prevStack_.empty() && prev->Rip != c.Rip;
    auto prevSlot = [&](uint64_t address, uint64_t& value) {
        if (address < prevStackRsp_ || address + ptr > prevStackRsp_ + prevStack_.size()) return false;
        value = 0;
        memcpy(&value, prevStack_.data() + (address - prevStackRsp_), ptr);
        return true;
    };
    if (fresh && prev->Rsp != c.Rsp) {
        int64_t delta = static_cast<int64_t>(c.Rsp - prev->Rsp);
        uint64_t top = 0, oldTop = 0;
        if (got) memcpy(&top, raw.data(), ptr);
        prevSlot(prev->Rsp, oldTop);
        std::string what;
        char n[32];
        sprintf_s(n, "%lld", static_cast<long long>(delta < 0 ? -delta : delta));
        if (delta == -ptr && top > prev->Rip && top <= prev->Rip + 64 && (c.Rip < prev->Rip || c.Rip > prev->Rip + 64)) {
            what = Tr(std::string("call: адрес возврата положен на вершину стека"));
        } else if (delta == -ptr) {
            what = Tr(std::string("push: значение положено на вершину стека"));
        } else if (delta == ptr && c.Rip == oldTop) {
            what = Tr(std::string("ret: адрес возврата снят со стека, переход по нему"));
        } else if (delta == ptr) {
            what = Tr(std::string("pop: значение снято с вершины стека"));
        } else if (delta < 0) {
            what = Tr(std::string("в стеке выделено место"));
        } else {
            what = Tr(std::string("место в стеке освобождено"));
        }
        r.Add(std::string(is32 ? "esp " : "rsp ") + (delta < 0 ? "− " : "+ ") + n + ":  " + what + "\n", P_CHANGED);
    }
    std::vector<int> flashLines;
    for (size_t i = 0; i < got; i++) {
        memcpy(&stack[i], raw.data() + i * ptr, ptr);
        uint64_t address = c.Rsp + i * ptr;
        uint64_t old = 0;
        if (fresh && (!prevSlot(address, old) || old != stack[i] || address < prev->Rsp)) flashLines.push_back(r.line);
        const Debugger::Frame* returnOf = nullptr;
        for (size_t k = 1; k < frames_.size(); k++) {
            if (frames_[k].returnSlot == address) returnOf = &frames_[k];
        }
        char label[24];
        sprintf_s(label, "%s+%02zX ", is32 ? "esp" : "rsp", i * ptr);
        r.Add(label, P_ADDR);
        r.Add(HexN(stack[i], digits), returnOf ? P_SYMBOL : P_TEXT);
        if (returnOf) {
            r.Add("  ← " + Tr(std::string("адрес возврата в ")) + Narrow(returnOf->function), P_SYMBOL);
        } else {
            annotate(stack[i]);
        }
        if (address == c.Rbp) r.Add(std::string("  ← ") + (is32 ? "ebp" : "rbp"), P_DIM);
        r.Add("\n", P_TEXT);
        if (returnOf) r.Add("  ── " + Tr(std::string("выше — кадр функции ")) + Narrow(returnOf->function) + " ──\n", P_DIM);
    }
    int xmmCount = is32 ? 8 : 16;
    r.Add("\n" + Tr(std::string("XMM (старшие · младшие 64 бита · младший double)")) + "\n", P_HEADER);
    for (int i = 0; i < xmmCount; i++) {
        const M128A& x = c.FltSave.XmmRegisters[i];
        bool changed = prev && (prev->FltSave.XmmRegisters[i].Low != x.Low || prev->FltSave.XmmRegisters[i].High != x.High);
        r.Add(PadRight("xmm" + std::to_string(i), 6), P_LABEL);
        r.Add(Hex16(static_cast<uint64_t>(x.High)) + " " + Hex16(x.Low), changed ? P_CHANGED : P_TEXT);
        double d;
        memcpy(&d, &x.Low, 8);
        char buf[48];
        sprintf_s(buf, "  %g\n", d);
        r.Add(buf, P_DIM);
    }
    RenderPanel(*regsView_, r.runs);
    // Remembered for the next stop: which slots changed, and what a pop or ret took.
    prevStack_.assign(raw.begin(), raw.end());
    prevStackRsp_ = c.Rsp;
    regsView_->Call(SCI_SETINDICATORCURRENT, 9);
    regsView_->Call(SCI_INDICATORCLEARRANGE, 0, regsView_->Call(SCI_GETLENGTH));
    for (int line : flashLines) {
        Sci_Position start = regsView_->Call(SCI_POSITIONFROMLINE, line);
        regsView_->Call(SCI_INDICATORFILLRANGE, start, regsView_->Call(SCI_GETLINEENDPOSITION, line) - start);
    }
    if (!flashLines.empty()) {
        stackFlash_ = 8;
        regsView_->Call(SCI_INDICSETALPHA, 9, 240);
        regsView_->Call(SCI_INDICSETOUTLINEALPHA, 9, 255);
        SetTimer(hwnd_, kStackFlashTimer, 90, nullptr);
    }
}

void MainWindow::RenderCallStack() {
    PanelText r;
    stackLines_.clear();
    if (!DebugStopped()) {
        RenderPanel(*stackView_, {{Tr(std::string("Стек вызовов виден, когда программа остановлена.\n")), P_DIM}});
        return;
    }
    r.Add(Tr(std::string("СТЕК ВЫЗОВОВ")) + "  " + Tr(std::string("(двойной клик — перейти к строке)")) + "\n", P_HEADER);
    stackLines_.push_back(-1);
    int digits = debugger_->Is32() ? 8 : 16;
    for (size_t i = 0; i < frames_.size(); i++) {
        const Debugger::Frame& f = frames_[i];
        stackLines_.push_back(static_cast<int>(i));
        r.Add(i == 0 ? "▶ " : "  ", i == 0 ? P_CURRENT : P_TEXT);
        r.Add("#" + std::to_string(i) + " ", P_ADDR);
        r.Add(f.function.empty() ? "0x" + HexN(f.pc, digits) : Narrow(f.function), i == 0 ? P_CURRENT : P_SYMBOL);
        if (f.location.valid()) {
            r.Add("   " + Narrow(f.location.file.filename().wstring()) + ":" + std::to_string(f.location.line), P_DIM);
        }
        r.Add("\n", P_TEXT);
    }
    r.Add("\n" + Tr(std::string("Кадры DLL и C runtime — по их таблицам раскрутки (.pdata), кадры NASM-кода — по адресам "
                                 "возврата в стеке.\n")), P_DIM);
    RenderPanel(*stackView_, r.runs);
}

void MainWindow::RenderWatches() {
    PanelText r;
    watchLines_.clear();
    auto mark = [&](int value) {
        if (static_cast<int>(watchLines_.size()) <= r.line) watchLines_.resize(r.line + 1, -1);
        watchLines_[r.line] = value;
    };
    r.Add(Tr(std::string("НАБЛЮДЕНИЕ")) + "  " + Tr(std::string("(двойной клик — изменить или удалить)")) + "\n", P_HEADER);
    bool stopped = DebugStopped();
    for (size_t i = 0; i < watches_.size(); i++) {
        mark(static_cast<int>(i));
        r.Add(watches_[i] + " = ", P_LABEL);
        if (!stopped) {
            r.Add("—\n", P_DIM);
            continue;
        }
        std::wstring error;
        auto v = debugger_->Evaluate(watches_[i], error);
        if (!v) {
            r.Add(Narrow(error) + "\n", P_ERROR);
            continue;
        }
        char buf[64];
        sprintf_s(buf, "0x%llX  (%lld)", static_cast<unsigned long long>(*v), static_cast<long long>(*v));
        r.Add(buf, P_TEXT);
        std::wstring sym = debugger_->SymbolAt(*v);
        if (!sym.empty()) {  // a label: also show what is stored there
            uint64_t q = 0;
            size_t got = debugger_->ReadMemory(*v, &q, 8);
            r.Add("  " + Narrow(sym), P_SYMBOL);
            if (got == 8) {
                sprintf_s(buf, "  [qword] = 0x%llX (%lld)", static_cast<unsigned long long>(q), static_cast<long long>(q));
                r.Add(buf, P_DIM);
            }
        }
        r.Add("\n", P_TEXT);
    }
    if (watches_.empty()) r.Add(Tr(std::string("Введите выражение в поле выше и нажмите Enter.\n")), P_DIM);

    r.Add("\n" + Tr(std::string("ТОЧКИ ОСТАНОВА НА ДАННЫЕ")) + "\n", P_HEADER);
    std::vector<DataWatch> data = debugger_ ? debugger_->DataWatches() : std::vector<DataWatch>{};
    for (size_t i = 0; i < data.size(); i++) {
        mark(-2 - static_cast<int>(i));
        char buf[96];
        sprintf_s(buf, " (%d %s) @ 0x%llX = 0x%llX\n", data[i].size, Tr(std::string("байт")).c_str(),
                  static_cast<unsigned long long>(data[i].address), static_cast<unsigned long long>(data[i].value));
        r.Add(data[i].expression, P_LABEL);
        r.Add(buf, P_TEXT);
    }
    if (data.empty()) {
        r.Add(Tr(std::string("Нет. Запуск → «Остановиться при изменении данных…» — остановка, когда программа\n"
                             "запишет в переменную (до 4 штук, через отладочные регистры DR0–DR3).\n")), P_DIM);
    }
    RenderPanel(*watchView_, r.runs);
}

// Disassembly of the current function (or from rip), with the source line above each group of instructions:
// shows what macros and pseudo-instructions became.
void MainWindow::RenderDisassembly() {
    disasmLines_.clear();
    if (!DebugStopped()) {
        RenderPanel(*disasmView_, {{Tr(std::string("Дизассемблер показывает код, когда программа остановлена.\n")), P_DIM}});
        return;
    }
    auto ndisasm = Toolchain::FindNdisasm(*settings_);
    if (!ndisasm) {
        RenderPanel(*disasmView_, {{Tr(std::string("Не найден ndisasm.exe (входит в NASM, ищется рядом с nasm.exe).\n")), P_ERROR}});
        return;
    }
    const CONTEXT& c = debugger_->Context();
    uint64_t start = c.Rip;
    std::wstring sym = debugger_->SymbolAt(c.Rip);
    size_t plus = sym.find(L"+0x");
    if (plus != std::wstring::npos) {
        uint64_t displacement = wcstoull(sym.c_str() + plus + 3, nullptr, 16);
        if (displacement < 0x800) start = c.Rip - displacement;  // from the label the code belongs to
    }
    std::vector<uint8_t> code(std::min<uint64_t>(c.Rip - start, 0x800) + 160);
    size_t got = debugger_->ReadMemory(start, code.data(), code.size());
    std::vector<Disasm::Line> lines = Disasm::Run(*ndisasm, code.data(), got, start, debugger_->Is32() ? 32 : 64);
    PanelText r;
    r.Add(Tr(std::string("КОД")) + "  " + Narrow(sym.empty() ? std::wstring() : sym.substr(0, plus)) + "  " +
              Tr(std::string("(двойной клик — к строке исходника)")) + "\n", P_HEADER);
    disasmLines_.push_back(0);
    int digits = debugger_->Is32() ? 8 : 12;
    SourceLocation last;
    int shownAfter = 0;
    for (const Disasm::Line& l : lines) {
        if (l.address > c.Rip && ++shownAfter > 24) break;
        SourceLocation loc = debugger_->LocationOf(l.address);
        if (loc.valid() && !(loc == last)) {  // the source line these instructions came from
            std::string source;
            int index = FindByPath(loc.file);
            if (index >= 0) source = editors_[index]->LineText(loc.line - 1);
            size_t first = source.find_first_not_of(" \t");
            source = first == std::string::npos ? std::string() : source.substr(first);
            while (!source.empty() && (source.back() == '\r' || source.back() == ' ')) source.pop_back();
            disasmLines_.push_back(l.address);
            r.Add(Narrow(loc.file.filename().wstring()) + ":" + std::to_string(loc.line) + "  " + source + "\n", P_DIM);
            last = loc;
        }
        bool current = l.address == c.Rip;
        disasmLines_.push_back(l.address);
        r.Add(current ? "▶ " : "  ", P_CURRENT);
        r.Add(HexN(l.address, digits) + "  ", current ? P_CURRENT : P_ADDR);
        r.Add(PadRight(l.bytes, 16) + " ", P_DIM);
        r.Add(l.text + "\n", current ? P_CURRENT : P_CODE);
    }
    if (lines.empty()) r.Add(Tr(std::string("Не удалось прочитать код.\n")), P_ERROR);
    RenderPanel(*disasmView_, r.runs);
    // Keep the current instruction in view.
    for (size_t i = 0; i < disasmLines_.size(); i++) {
        if (disasmLines_[i] == c.Rip) {
            disasmView_->Call(SCI_SETFIRSTVISIBLELINE, std::max<int>(0, static_cast<int>(i) - 6));
            break;
        }
    }
}

void MainWindow::RenderMemory() {
    if (!DebugStopped()) return;
    int n = GetWindowTextLengthW(memExpr_);
    std::wstring w(n, L'\0');
    GetWindowTextW(memExpr_, w.data(), n + 1);
    std::string expr = Narrow(Trim(w));
    if (expr.empty()) expr = "rsp";
    PanelText r;
    memLines_.clear();
    std::wstring error;
    auto addr = debugger_->Evaluate(expr, error);
    if (!addr) {
        RenderPanel(*memView_, {{Narrow(error) + "\n", P_ERROR}});
        return;
    }
    r.Add(expr + " = 0x" + Hex16(*addr), P_TEXT);
    std::wstring sym = debugger_->SymbolAt(*addr);
    if (!sym.empty()) r.Add("  " + Narrow(sym), P_SYMBOL);
    r.Add("   " + Tr(std::string("(двойной клик — изменить)")) + "\n", P_DIM);
    memLines_.push_back(0);
    std::vector<uint8_t> bytes(256);
    size_t got = debugger_->ReadMemory(*addr, bytes.data(), bytes.size());
    bytes.resize(got);
    // Bytes that differ from the previous stop (same address) are highlighted.
    bool compare = prevMemAddress_ == *addr && !prevMem_.empty();
    auto changed = [&](size_t from, size_t count) {
        if (!compare) return false;
        for (size_t i = from; i < from + count; i++) {
            if (i < prevMem_.size() && i < bytes.size() && prevMem_[i] != bytes[i]) return true;
        }
        return false;
    };
    if (got == 0) r.Add(Tr(std::string("память недоступна\n")), P_ERROR);
    if (memFormat_ == MemFormat::Text) {  // characters, a line per 32 bytes
        for (size_t row = 0; row < got; row += 32) {
            memLines_.push_back(*addr + row);
            r.Add(Hex16(*addr + row) + "  ", P_ADDR);
            std::string text = "\"";
            for (size_t i = row; i < std::min(got, row + 32); i++) {
                unsigned char ch = bytes[i];
                if (ch >= 32 && ch < 127) text += static_cast<char>(ch);
                else if (ch == 0) text += "\\0";
                else if (ch == 10) text += "\\n";
                else if (ch == 13) text += "\\r";
                else {
                    char b[8];
                    sprintf_s(b, "\\x%02X", ch);
                    text += b;
                }
            }
            r.Add(text + "\"\n", changed(row, 32) ? P_CHANGED : P_TEXT);
        }
    } else {
        int size = memFormat_ == MemFormat::Words ? 2 : memFormat_ == MemFormat::Dwords || memFormat_ == MemFormat::Float ? 4
                 : memFormat_ == MemFormat::Bytes ? 1 : 8;
        int perRow = memFormat_ == MemFormat::Bytes ? 8 : memFormat_ == MemFormat::Words ? 4
                   : memFormat_ == MemFormat::Qwords || memFormat_ == MemFormat::Double ? 1 : 2;
        int rowBytes = size * perRow;
        for (size_t row = 0; row + size <= got; row += rowBytes) {
            memLines_.push_back(*addr + row);
            r.Add(Hex16(*addr + row) + "  ", P_ADDR);
            std::string ascii;
            for (int k = 0; k < perRow && row + (k + 1) * size <= got; k++) {
                size_t at = row + k * size;
                uint64_t v = 0;
                memcpy(&v, bytes.data() + at, size);
                char b[64];
                switch (memFormat_) {
                case MemFormat::Bytes: sprintf_s(b, "%02X ", static_cast<unsigned>(v)); break;
                case MemFormat::Words: sprintf_s(b, "%04X (%6d)  ", static_cast<unsigned>(v), static_cast<int16_t>(v)); break;
                case MemFormat::Dwords: sprintf_s(b, "%08X (%11d)  ", static_cast<unsigned>(v), static_cast<int32_t>(v)); break;
                case MemFormat::Qwords:
                    sprintf_s(b, "%016llX (%lld)", static_cast<unsigned long long>(v), static_cast<long long>(v));
                    break;
                case MemFormat::Float: {
                    float f;
                    memcpy(&f, &v, 4);
                    sprintf_s(b, "%-14g  ", f);
                    break;
                }
                default: {
                    double d;
                    memcpy(&d, &v, 8);
                    sprintf_s(b, "%.17g", d);
                    break;
                }
                }
                r.Add(b, changed(at, size) ? P_CHANGED : P_TEXT);
                for (int i = 0; i < size; i++) {
                    unsigned char ch = bytes[at + i];
                    ascii += (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '.';
                }
            }
            r.Add(memFormat_ == MemFormat::Bytes ? " " + ascii + "\n" : "\n", P_DIM);
        }
    }
    prevMem_ = bytes;
    prevMemAddress_ = *addr;
    RenderPanel(*memView_, r.runs);
}

// Double click in a debug panel: edit a register / memory / watch, or go to a frame's source line.
void MainWindow::PanelDoubleClick(Editor& view, Sci_Position pos) {
    int line = static_cast<int>(view.Call(SCI_LINEFROMPOSITION, pos));
    if (&view == regsView_.get()) {
        if (line >= static_cast<int>(regLines_.size()) || regLines_[line].empty()) return;
        if (regLines_[line] == "flags") {  // toggle the flag under the mouse
            std::string word = view.WordAt(pos);
            static const std::pair<const char*, int> flags[] = {{"CF", 0}, {"PF", 2}, {"AF", 4}, {"ZF", 6}, {"SF", 7},
                                                                {"DF", 10}, {"OF", 11}};
            for (auto [name, bit] : flags) {
                if (word.rfind(name, 0) == 0 && DebugStopped()) {
                    debugger_->SetRegister("rflags", debugger_->Context().EFlags ^ (1u << bit));
                    RenderRegisters();
                }
            }
            return;
        }
        EditRegister(regLines_[line]);
    } else if (&view == memView_.get()) {
        if (line > 0 && line < static_cast<int>(memLines_.size())) {
            // Which element of the row: count the values before the click.
            Sci_Position lineStart = view.Call(SCI_POSITIONFROMLINE, line);
            std::string text = view.LineText(line);
            int column = static_cast<int>(pos - lineStart);
            int size = memFormat_ == MemFormat::Words ? 2 : memFormat_ == MemFormat::Dwords || memFormat_ == MemFormat::Float ? 4
                     : memFormat_ == MemFormat::Bytes || memFormat_ == MemFormat::Text ? 1 : 8;
            int index = 0;
            if (memFormat_ == MemFormat::Bytes) index = std::clamp((column - 18) / 3, 0, 7);
            else if (memFormat_ == MemFormat::Words) index = std::clamp((column - 18) / 16, 0, 3);
            else if (memFormat_ == MemFormat::Dwords) index = std::clamp((column - 18) / 23, 0, 1);
            else if (memFormat_ == MemFormat::Float) index = std::clamp((column - 18) / 16, 0, 1);
            EditMemory(memLines_[line] + static_cast<uint64_t>(index) * size);
        }
    } else if (&view == flagsView_.get()) {
        if (line < static_cast<int>(flagLines_.size()) && flagLines_[line] >= 0 && DebugStopped()) {
            debugger_->SetRegister("rflags", debugger_->Context().EFlags ^ (1u << flagLines_[line]));
            RenderFlags();
            RenderRegisters();
        }
    } else if (&view == stackView_.get()) {
        if (line < static_cast<int>(stackLines_.size()) && stackLines_[line] >= 0) {
            const Debugger::Frame& f = frames_[stackLines_[line]];
            if (f.location.valid()) OpenFile(f.location.file, f.location.line);
        }
    } else if (&view == watchView_.get()) {
        if (line >= static_cast<int>(watchLines_.size())) return;
        int w = watchLines_[line];
        if (w >= 0 && w < static_cast<int>(watches_.size())) {
            auto text = InputBox(hwnd_, Tr(L"Наблюдение"), Tr(L"Выражение (пусто — удалить):"), Widen(watches_[w]));
            if (!text) return;
            if (Trim(*text).empty()) watches_.erase(watches_.begin() + w);
            else watches_[w] = Narrow(Trim(*text));
            SaveProjectSession();
            RenderWatches();
        } else if (w <= -2 && debugger_) {
            debugger_->RemoveDataWatch(static_cast<size_t>(-2 - w));
            RenderWatches();
        }
    } else if (&view == disasmView_.get()) {
        if (line < static_cast<int>(disasmLines_.size()) && disasmLines_[line] && debugger_) {
            SourceLocation loc = debugger_->LocationOf(disasmLines_[line]);
            if (loc.valid()) OpenFile(loc.file, loc.line);
        }
    }
}

void MainWindow::EditRegister(const std::string& name) {
    if (!DebugStopped()) return;
    auto current = Debugger::Register(debugger_->Context(), name);
    if (!current) return;
    char buf[32];
    sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(*current));
    auto text = InputBox(hwnd_, Tr(L"Изменить регистр"),
                         Widen(name) + L" = (" + Tr(L"число, 0x…, или выражение: rax+1, [rsp]") + L")", Widen(buf));
    if (!text || Trim(*text).empty()) return;
    std::wstring error;
    auto v = debugger_->Evaluate(Narrow(Trim(*text)), error);
    if (!v) {
        ShowMessage(hwnd_, error);
        return;
    }
    debugger_->SetRegister(name, *v);
    PrintTo(Pane::DebugOutput, name + " := 0x" + Hex16(*v) + "\n", Out::Info);
    if (name == "rip" || name == "eip") {
        SourceLocation loc = debugger_->LocationOf(*v);
        ClearExecutionLine();
        if (loc.valid()) {
            if (Editor* ed = OpenFile(loc.file, loc.line)) ed->SetExecutionLine(loc.line);
        }
    }
    RenderDebugPanel();
}

// New contents for memory: hex bytes ("48 65 6C"), a string in quotes, or a number of the shown size.
void MainWindow::EditMemory(uint64_t address) {
    if (!DebugStopped()) return;
    int size = memFormat_ == MemFormat::Words ? 2 : memFormat_ == MemFormat::Dwords || memFormat_ == MemFormat::Float ? 4
             : memFormat_ == MemFormat::Qwords || memFormat_ == MemFormat::Double ? 8 : 1;
    std::wstring prompt = std::wstring(Tr(L"Записать по адресу ")) + Widen("0x" + Hex16(address)) + L":\n" +
                          (size == 1 ? Tr(L"байты в hex (48 65 6C) или строку в кавычках (\"текст\")")
                                     : Tr(L"число (десятичное, 0x… или с точкой для float/double)"));
    auto text = InputBox(hwnd_, Tr(L"Изменить память"), prompt, L"");
    if (!text || Trim(*text).empty()) return;
    std::string input = Narrow(Trim(*text));
    std::vector<uint8_t> data;
    if (input.size() >= 2 && (input[0] == '"' || input[0] == '\'') && input.back() == input[0]) {
        data.assign(input.begin() + 1, input.end() - 1);
    } else if (size == 1) {
        std::istringstream in(input);
        std::string token;
        while (in >> token) data.push_back(static_cast<uint8_t>(strtoul(token.c_str(), nullptr, 16)));
    } else if (memFormat_ == MemFormat::Float || memFormat_ == MemFormat::Double) {
        double d = atof(input.c_str());
        if (memFormat_ == MemFormat::Float) {
            float f = static_cast<float>(d);
            data.resize(4);
            memcpy(data.data(), &f, 4);
        } else {
            data.resize(8);
            memcpy(data.data(), &d, 8);
        }
    } else {
        std::wstring error;
        auto v = debugger_->Evaluate(input, error);
        if (!v) {
            ShowMessage(hwnd_, error);
            return;
        }
        data.resize(size);
        memcpy(data.data(), &*v, size);
    }
    if (data.empty()) return;
    if (!debugger_->WriteMemory(address, data.data(), data.size())) {
        ShowMessage(hwnd_, Tr(L"Не удалось записать в память по этому адресу."));
        return;
    }
    PrintTo(Pane::DebugOutput, Tr(std::string("записано байт: ")) + std::to_string(data.size()) + " @ 0x" + Hex16(address) + "\n",
            Out::Info);
    RenderDebugPanel();
}

void MainWindow::AddWatch(const std::string& expression) {
    watches_.push_back(expression);
    SaveProjectSession();
    if (!DebugPanelVisible()) {
        showDebugPanel_ = true;
        CheckMenuItem(viewMenu_, ID_DEBUG_PANEL, MF_CHECKED);
    }
    debugTab_ = DebugTab::Watch;
    Layout();
    RenderWatches();
}

// "counter" or "counter, 4": stop when the program writes to those bytes.
void MainWindow::AddDataBreakpoint(const std::string& text) {
    if (!DebugStopped()) {
        ShowMessage(hwnd_, Tr(L"Точку останова на данные можно поставить, когда программа остановлена в отладчике "
                              L"(адрес переменной известен только у запущенной программы)."), false);
        return;
    }
    std::string expr = text;
    int size = debugger_->PointerSize();
    size_t comma = text.rfind(',');
    if (comma != std::string::npos) {
        size = atoi(text.c_str() + comma + 1);
        expr = text.substr(0, comma);
    }
    while (!expr.empty() && expr.back() == ' ') expr.pop_back();
    std::wstring error;
    auto address = debugger_->Evaluate(expr, error);
    if (!address) {
        ShowMessage(hwnd_, error);
        return;
    }
    DataWatch w;
    w.expression = expr;
    w.address = *address;
    w.size = size;
    if (!debugger_->AddDataWatch(w, error)) {
        ShowMessage(hwnd_, error);
        return;
    }
    PrintTo(Pane::DebugOutput, Tr(std::string("точка останова на данные: ")) + expr + " @ 0x" + Hex16(*address) + "\n", Out::Info);
    debugTab_ = DebugTab::Watch;
    Layout();
    RenderWatches();
}

std::string MainWindow::DebugTipText(Editor* ed, Sci_Position pos) {
    if (pos < 0 || !DebugStopped()) return {};
    // Hovering a selection: its value as an expression ("[rsp+8]", "rax*2+1", "dword [counter]").
    Sci_Position selStart = ed->Call(SCI_GETSELECTIONSTART), selEnd = ed->Call(SCI_GETSELECTIONEND);
    if (selEnd > selStart && pos >= selStart && pos <= selEnd && selEnd - selStart < 200) {
        std::string expr(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, selStart, selEnd - selStart)),
                         selEnd - selStart);
        if (expr.find('\n') == std::string::npos) {
            std::wstring error;
            auto v = debugger_->Evaluate(Narrow(Trim(Widen(expr))), error);
            if (!v) return expr + ": " + Narrow(Tr(error.c_str()));
            char buf[96];
            sprintf_s(buf, " = 0x%llX  (%lld)", static_cast<unsigned long long>(*v), static_cast<long long>(*v));
            std::string tip = Narrow(Trim(Widen(expr))) + buf;
            std::wstring sym = debugger_->SymbolAt(*v);
            if (!sym.empty()) tip += "  " + Narrow(sym);
            return tip;
        }
    }
    std::string word = ed->WordAt(pos);
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0]))) return {};
    std::string lower = LowerAscii(word);
    std::string tip;
    if (auto reg = Debugger::Register(debugger_->Context(), lower)) {
        char buf[64];
        sprintf_s(buf, " = 0x%llX  (%lld)", static_cast<unsigned long long>(*reg), static_cast<long long>(*reg));
        tip = word + buf;
    } else if (auto addr = debugger_->LookupSymbol(word)) {
        uint8_t bytes[16];
        size_t got = debugger_->ReadMemory(*addr, bytes, sizeof(bytes));
        tip = word + " @ 0x" + Hex16(*addr);
        if (got >= 8) {
            uint64_t q;
            memcpy(&q, bytes, 8);
            tip += "\nqword: 0x" + Hex16(q) + "  (" + std::to_string(static_cast<int64_t>(q)) + ")";
        }
        if (got) {
            std::string hex, ascii;
            for (size_t i = 0; i < got; i++) {
                char b[4];
                sprintf_s(b, "%02X ", bytes[i]);
                hex += b;
                ascii += (bytes[i] >= 32 && bytes[i] < 127) ? static_cast<char>(bytes[i]) : '.';
            }
            tip += "\n" + Tr(std::string("байты: ")) + hex + " " + ascii;
        }
    }
    return tip;
}

// What a user-defined name stands for: the body of a %macro, the value of %define / equ.
std::string MainWindow::SymbolHint(Editor* ed, const std::string& word) {
    if (word.empty()) return {};
    std::string name = word[0] == '%' ? word.substr(1) : word;
    auto linesOf = [&](Editor* e, const fs::path& file) {
        std::string text;
        if (e) {
            text = e->Text();
        } else {
            std::ifstream in(file, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        std::vector<std::string> lines;
        std::istringstream in(text);
        std::string l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
        return lines;
    };
    auto describe = [&](const std::vector<std::string>& lines, int line) -> std::string {
        if (line < 1 || line > static_cast<int>(lines.size())) return {};
        std::string def = lines[line - 1];
        size_t first = def.find_first_not_of(" \t");
        def = first == std::string::npos ? std::string() : def.substr(first);
        std::string lower = LowerAscii(def);
        if (lower.rfind("%macro", 0) == 0 || lower.rfind("%imacro", 0) == 0) {
            std::string body = def;
            for (int i = line; i < static_cast<int>(lines.size()) && i < line + 14; i++) {
                body += "\n" + lines[i];
                if (LowerAscii(lines[i]).find("%endmacro") != std::string::npos) break;
            }
            return Tr(std::string("Макрос")) + " " + name + "\n" + body;
        }
        size_t equ = lower.find(" equ ");
        if (equ == std::string::npos) equ = lower.find("\tequ ");
        std::string value;
        if (equ != std::string::npos) {
            value = def.substr(equ + 5);
        } else if (lower[0] == '%') {  // %define NAME value
            size_t at = def.find(name);
            if (at != std::string::npos) value = def.substr(at + name.size());
        } else {
            return {};
        }
        size_t comment = value.find(';');
        if (comment != std::string::npos) value.erase(comment);
        size_t a = value.find_first_not_of(" \t"), b = value.find_last_not_of(" \t");
        value = a == std::string::npos ? std::string() : value.substr(a, b - a + 1);
        std::string tip = name + " = " + value;
        char* end = nullptr;
        long long n = strtoll(value.c_str(), &end, 0);
        if (!value.empty() && end && *end == 0) {
            char buf[64];
            sprintf_s(buf, "  (0x%llX)", static_cast<unsigned long long>(n));
            tip += buf;
        }
        return tip + "\n" + def;
    };
    for (const auto& s : Completion::ScanSymbols(ed->Text())) {
        if (s.name == name && !s.declaration) {
            std::string tip = describe(linesOf(ed, {}), s.line);
            if (!tip.empty()) return tip;
        }
    }
    for (const auto& hit : ProjectSymbols(ed)) {
        if (hit.symbol.name == name && !hit.symbol.declaration) {
            std::string tip = describe(linesOf(hit.editor, hit.file), hit.symbol.line);
            if (!tip.empty()) return tip + "\n(" + Narrow(hit.file.filename().wstring()) + ")";
        }
    }
    return {};
}

// Reference card for the word under the mouse or caret: instructions, registers, directives, C / WinAPI functions,
// user macros and constants (with the register's or variable's value while the debugger is stopped).
void MainWindow::ShowHint(Editor* ed, Sci_Position pos) {
    if (pos < 0) return;
    Sci_Position start, end;
    if (!ed->WordRange(pos, start, end)) return;
    std::string word = ed->WordAt(pos);
    if (start > 0 && ed->Call(SCI_GETCHARAT, start - 1) == '%') {  // %define, %macro…
        word = "%" + word;
        start--;
    }
    int style = static_cast<int>(ed->Call(SCI_GETSTYLEAT, start));
    std::string card;
    size_t header = 0;
    if (style == T_INSTRUCTION || style == T_REGISTER || style == T_DIRECTIVE || style == T_PREPROCESSOR ||
        style == T_KEYWORD) {
        card = Hints::For(word);
        header = Hints::HeaderLength(card);
    }
    if (card.empty() && (style == T_DEFAULT || style == T_LABEL || style == T_PREPROCESSOR)) {
        card = Hints::Function(word);
        if (!card.empty()) header = Hints::HeaderLength(card);
        std::string args = CallArgumentsText(ed, static_cast<int>(ed->Call(SCI_LINEFROMPOSITION, start)));
        if (!card.empty() && !args.empty()) card += "\n\n" + args;
        else card = SymbolHint(ed, word);
        if (!card.empty() && !header) header = card.find('\n') == std::string::npos ? card.size() : card.find('\n');
    }
    std::string debug = DebugTipText(ed, pos);
    if (card.empty() && debug.empty()) return;
    std::string tip = debug.empty() ? card : card.empty() ? debug : debug + "\n\n" + card;
    ed->Call(SCI_CALLTIPSHOW, start, reinterpret_cast<sptr_t>(tip.c_str()));
    if (!card.empty()) {
        size_t from = debug.empty() ? 0 : debug.size() + 2;
        ed->Call(SCI_CALLTIPSETHLT, from, from + header);
    }
}

// After "mnemonic " is typed: a one-line reminder of the operands.
void MainWindow::ShowSignature(Editor* ed) {
    Sci_Position pos = ed->Call(SCI_GETCURRENTPOS);
    Sci_Position lineStart = ed->Call(SCI_POSITIONFROMLINE, ed->Call(SCI_LINEFROMPOSITION, pos));
    std::string before(reinterpret_cast<const char*>(ed->Call(SCI_GETRANGEPOINTER, lineStart, pos - lineStart)),
                       pos - lineStart);
    size_t colon = before.find(':');
    size_t from = colon != std::string::npos && before.find_first_of(";\"'") > colon ? colon + 1 : 0;
    size_t s = before.find_first_not_of(" \t", from);
    if (s == std::string::npos) return;
    size_t e = before.find_first_of(" \t", s);
    if (e == std::string::npos || before.find_first_not_of(" \t", e) != std::string::npos) return;  // operands begun
    std::string mnemonic = before.substr(s, e - s);
    std::string sig = Hints::Signature(mnemonic);
    if (sig.empty()) return;
    ed->Call(SCI_CALLTIPSHOW, lineStart + s, reinterpret_cast<sptr_t>(sig.c_str()));
    ed->Call(SCI_CALLTIPSETHLT, 0, Hints::HeaderLength(sig));
}
