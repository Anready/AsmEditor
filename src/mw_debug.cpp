#include "mw_internal.h"
#include "hints.h"

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
    debugStatus_ = L"выполняется…";
    debugStatusColor_ = Theme::Success;
    switch (id) {
    case ID_DEBUG: debugger_->Continue(); break;
    case ID_STEP_OVER: debugger_->StepOver(); break;
    case ID_STEP_INTO: debugger_->StepInto(); break;
    case ID_STEP_OUT: debugger_->StepOut(); break;
    }
    // Stopped() turns false on the debugger thread; repaint once it has picked up the command.
    InvalidateChrome();
    SetTimer(hwnd_, 1, 50, [](HWND h, UINT, UINT_PTR id, DWORD) {
        KillTimer(h, id);
        InvalidateRect(h, nullptr, FALSE);
    });
    // If the program keeps running (e.g. waits in scanf), typing should go to it, not into the source.
    SetTimer(hwnd_, kConsoleFocusTimer, 400, nullptr);
}

void MainWindow::SetDebugging(bool on) {
    debugging_ = on;
    haveLastContext_ = false;
    if (on) {
        debugStatus_ = L"выполняется…";
        debugStatusColor_ = Theme::Success;
        RenderPanel(*regsView_, {{"Программа выполняется.\nПоставьте точку останова (F9 или клик слева от номера "
                                  "строки) или нажмите «Пауза».\n", P_DIM}});
        RenderPanel(*memView_, {});
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnDebugStopped(std::unique_ptr<DebugStop> stop) {
    if (!debugging_) return;
    switch (stop->reason) {
    case DebugStop::Reason::Breakpoint: debugStatus_ = L"точка останова"; break;
    case DebugStop::Reason::Step: debugStatus_ = L"шаг"; break;
    case DebugStop::Reason::Entry: debugStatus_ = L"начало main"; break;
    case DebugStop::Reason::Pause: debugStatus_ = L"пауза"; break;
    case DebugStop::Reason::Int3: debugStatus_ = L"int3 в программе"; break;
    case DebugStop::Reason::Exception: debugStatus_ = L"исключение"; break;
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
        Print(Narrow(stop->message + L"\n    в " + where) + "\n", Out::Error);
    } else if (!stop->location.valid()) {
        Print("Остановлено вне исходного кода: " + Narrow(where) + "\n", Out::Info);
    }

    ClearExecutionLine();
    if (stop->location.valid()) {
        if (Editor* ed = OpenFile(stop->location.file, stop->location.line)) {
            ed->SetExecutionLine(stop->location.line);
            execFile_ = stop->location.file;
        }
    }
    RenderRegisters();
    RenderMemory();
    lastContext_ = stop->context;
    haveLastContext_ = true;
    if (GetForegroundWindow() != hwnd_) SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnDebugExited(DWORD code) {
    if (!debugging_) return;
    debugger_.reset();
    ClearExecutionLine();
    SetDebugging(false);
    FinishConsole(code);
}

void MainWindow::ClearExecutionLine() {
    for (auto& e : editors_) e->SetExecutionLine(0);
    execFile_.clear();
}

void MainWindow::ToggleBreakpoint(Editor* ed, int line) {
    if (!ed || line <= 0) return;
    ed->ToggleBreakpoint(line);
    if (debugger_ && !ed->file().empty()) debugger_->SetBreakpoints(ed->file(), ed->Breakpoints());
}

void MainWindow::StashBreakpoints(Editor* ed) {
    if (!ed || ed->file().empty()) return;
    std::set<int> lines = ed->Breakpoints();
    if (lines.empty()) breakpoints_.erase(Debugger::Key(ed->file()));
    else breakpoints_[Debugger::Key(ed->file())] = lines;
}

Debugger::Breakpoints MainWindow::CollectBreakpoints() const {
    Debugger::Breakpoints all = breakpoints_;
    for (const auto& e : editors_) {
        if (e->file().empty()) continue;
        std::set<int> lines = e->Breakpoints();
        if (lines.empty()) all.erase(Debugger::Key(e->file()));
        else all[Debugger::Key(e->file())] = lines;
    }
    return all;
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

void MainWindow::RenderRegisters() {
    if (!DebugStopped()) return;
    const CONTEXT& c = debugger_->Context();
    const CONTEXT* prev = haveLastContext_ ? &lastContext_ : nullptr;
    Runs r;
    auto annotate = [&](uint64_t v) {
        std::wstring sym = debugger_->SymbolAt(v);
        if (!sym.empty()) r.push_back({"  " + Narrow(sym), P_SYMBOL});
        else if (v <= 0xFFFFF) r.push_back({"  " + std::to_string(v), P_DIM});
        else if (static_cast<int64_t>(v) < 0 && static_cast<int64_t>(v) > -0x100000)
            r.push_back({"  " + std::to_string(static_cast<int64_t>(v)), P_DIM});
    };

    r.push_back({"РЕГИСТРЫ\n", P_HEADER});
    static const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rip"};
    for (const char* n : names) {
        uint64_t v = *Debugger::Register(c, n);
        bool changed = prev && *Debugger::Register(*prev, n) != v;
        r.push_back({PadRight(n, 4), P_LABEL});
        r.push_back({Hex16(v), changed ? P_CHANGED : P_TEXT});
        annotate(v);
        r.push_back({"\n", P_TEXT});
    }
    r.push_back({"flg ", P_LABEL});
    static const std::pair<const char*, int> flags[] = {{"CF", 0}, {"PF", 2}, {"AF", 4}, {"ZF", 6}, {"SF", 7},
                                                        {"TF", 8}, {"IF", 9}, {"DF", 10}, {"OF", 11}};
    for (auto [name, bit] : flags) {
        bool on = (c.EFlags >> bit) & 1;
        bool changed = prev && (((prev->EFlags >> bit) & 1) != on);
        r.push_back({std::string(name) + (on ? "=1 " : "=0 "), changed ? P_CHANGED : on ? P_FLAG_ON : P_DIM});
    }
    r.push_back({"\n\nСТЕК\n", P_HEADER});
    uint64_t stack[16]{};
    size_t got = debugger_->ReadMemory(c.Rsp, stack, sizeof(stack)) / 8;
    for (size_t i = 0; i < got; i++) {
        char label[16];
        sprintf_s(label, "rsp+%02zX ", i * 8);
        r.push_back({label, P_ADDR});
        r.push_back({Hex16(stack[i]), P_TEXT});
        annotate(stack[i]);
        if (c.Rsp + i * 8 == c.Rbp) r.push_back({"  ← rbp", P_DIM});
        r.push_back({"\n", P_TEXT});
    }
    r.push_back({"\nXMM (старшие · младшие 64 бита · младший double)\n", P_HEADER});
    for (int i = 0; i < 16; i++) {
        const M128A& x = c.FltSave.XmmRegisters[i];
        bool changed = prev && (prev->FltSave.XmmRegisters[i].Low != x.Low || prev->FltSave.XmmRegisters[i].High != x.High);
        r.push_back({PadRight("xmm" + std::to_string(i), 6), P_LABEL});
        r.push_back({Hex16(static_cast<uint64_t>(x.High)) + " " + Hex16(x.Low), changed ? P_CHANGED : P_TEXT});
        double d;
        memcpy(&d, &x.Low, 8);
        char buf[48];
        sprintf_s(buf, "  %g\n", d);
        r.push_back({buf, P_DIM});
    }
    RenderPanel(*regsView_, r);
}

void MainWindow::RenderMemory() {
    if (!DebugStopped()) return;
    int n = GetWindowTextLengthW(memExpr_);
    std::wstring w(n, L'\0');
    GetWindowTextW(memExpr_, w.data(), n + 1);
    std::string expr = Narrow(Trim(w));
    if (expr.empty()) expr = "rsp";
    Runs r;
    std::wstring error;
    auto addr = debugger_->Evaluate(expr, error);
    if (!addr) {
        RenderPanel(*memView_, {{Narrow(error) + "\n", P_ERROR}});
        return;
    }
    r.push_back({expr + " = 0x" + Hex16(*addr), P_TEXT});
    std::wstring sym = debugger_->SymbolAt(*addr);
    if (!sym.empty()) r.push_back({"  " + Narrow(sym), P_SYMBOL});
    r.push_back({"\n", P_TEXT});
    uint8_t bytes[256];
    size_t got = debugger_->ReadMemory(*addr, bytes, sizeof(bytes));
    if (got == 0) r.push_back({"память недоступна\n", P_ERROR});
    for (size_t row = 0; row < got; row += 8) {
        r.push_back({Hex16(*addr + row) + "  ", P_ADDR});
        std::string hex, ascii;
        for (size_t i = 0; i < 8; i++) {
            if (row + i < got) {
                char b[4];
                sprintf_s(b, "%02X ", bytes[row + i]);
                hex += b;
                unsigned char ch = bytes[row + i];
                ascii += (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '.';
            } else {
                hex += "   ";
            }
        }
        r.push_back({hex, P_TEXT});
        r.push_back({" " + ascii + "\n", P_DIM});
    }
    RenderPanel(*memView_, r);
}

std::string MainWindow::DebugTipText(Editor* ed, Sci_Position pos) {
    if (pos < 0 || !DebugStopped()) return {};
    std::string word = ed->WordAt(pos);
    if (word.empty() || isdigit(static_cast<unsigned char>(word[0]))) return {};
    std::string lower = word;
    for (char& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
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
            tip += "\nбайты: " + hex + " " + ascii;
        }
    }
    return tip;
}

// Reference card for the instruction / register / directive under the mouse or caret (with the register's value
// while the debugger is stopped).
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
    if (style == T_INSTRUCTION || style == T_REGISTER || style == T_DIRECTIVE || style == T_PREPROCESSOR ||
        style == T_KEYWORD) {
        card = Hints::For(word);
    }
    std::string debug = DebugTipText(ed, pos);
    if (card.empty() && debug.empty()) return;
    std::string tip = debug.empty() ? card : card.empty() ? debug : debug + "\n\n" + card;
    ed->Call(SCI_CALLTIPSHOW, start, reinterpret_cast<sptr_t>(tip.c_str()));
    if (!card.empty()) {
        size_t from = debug.empty() ? 0 : debug.size() + 2;
        ed->Call(SCI_CALLTIPSETHLT, from, from + Hints::HeaderLength(card));
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
