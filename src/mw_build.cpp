#include "mw_internal.h"
#include "project.h"

#include <consoleapi.h>

// ---- build & run ------------------------------------------------------------------------------------------------

std::vector<fs::path> MainWindow::IncludeDirs(const fs::path& file) const {
    std::vector<fs::path> dirs;
    if (!file.empty()) dirs.push_back(file.parent_path());
    if (!projectRoot_.empty() && (dirs.empty() || !SamePath(dirs[0], projectRoot_))) dirs.push_back(projectRoot_);
    return dirs;
}

fs::path MainWindow::BuildTarget() {
    if (!mainFile_.empty() && FileExists(mainFile_.wstring())) return mainFile_;
    Editor* ed = Active();
    if (!ed) {
        ShowMessage(hwnd_, L"Откройте .asm файл или выберите главный файл проекта "
                           L"(ПКМ по файлу в дереве → «Сделать главным файлом»).", false);
        return {};
    }
    if (ed->file().empty() && !SaveAs(ed)) return {};
    return ed->file();
}

void MainWindow::StartBuild(After after, bool rebuild) {
    if (building_ || debugging_) return;
    if (!Toolchain::NasmAvailable(*settings_)) {
        ShowMessage(hwnd_, L"NASM не найден. Укажите путь к nasm.exe в настройках.");
        OnCommand(ID_SETTINGS);
        return;
    }
    fs::path src = BuildTarget();
    if (src.empty()) return;
    StopProgram();
    SaveAll();
    fs::path outDir = (IsUnder(src, projectRoot_) ? projectRoot_ : src.parent_path()) / L"build";
    OnCommand(ID_CLEAR_CONSOLE);
    building_ = true;
    InvalidateChrome();
    Settings copy = *settings_;
    BuildPlan plan;
    plan.outDir = outDir;
    plan.rebuild = rebuild;
    plan.exe = outDir / (src.stem().wstring() + L".exe");
    if (IsUnder(src, projectRoot_)) {  // a project: link all its source files into one program
        Project::Sources sources = Project::Collect(projectRoot_, src, excludedFromBuild_);
        plan.sources = sources.files;
        plan.root = projectRoot_;
        plan.includes = {projectRoot_};
        for (const fs::path& other : sources.otherPrograms) {
            Print("Пропущен (своя функция main): " + Narrow(other.filename().wstring()) + "\n", Out::Info);
        }
    } else {
        plan.sources = {src};
        plan.root = src.parent_path();
    }
    HWND hwnd = hwnd_;
    int afterCode = static_cast<int>(after);
    std::thread([copy, plan, src, hwnd, afterCode] {
        auto sink = [hwnd](const std::string& text, Out style) {
            PostMessageW(hwnd, WM_APP_OUTPUT, static_cast<WPARAM>(style), reinterpret_cast<LPARAM>(new std::string(text)));
        };
        auto* done = new BuildDone{Toolchain::Build(copy, plan, sink), afterCode, src};
        PostMessageW(hwnd, WM_APP_BUILD_DONE, 0, reinterpret_cast<LPARAM>(done));
    }).detach();
}

void MainWindow::OnBuildDone(BuildResult* r, After after, const fs::path& src) {
    building_ = false;
    for (auto& e : editors_) {
        if (e->file().empty()) continue;
        std::vector<Diagnostic> mine;
        for (const Diagnostic& d : r->diagnostics) {
            if (SamePath(d.file, e->file())) mine.push_back(d);
        }
        if (!mine.empty() || SamePath(e->file(), src)) e->SetDiagnostics(mine);
    }
    for (const Diagnostic& d : r->diagnostics) {
        if (d.severity == Severity::Error && FileExists(d.file.wstring())) {
            OpenFile(d.file, d.line);
            break;
        }
    }
    RefreshTree();
    InvalidateChrome();
    if (r->ok && after != After::Nothing) RunProgram(r->exe, IsUnder(src, projectRoot_) ? projectRoot_ : src.parent_path(), after);
}

UINT MainWindow::ProgramCodePage() const {
    if (settings_->programEncoding == L"cp866") return 866;
    if (settings_->programEncoding == L"windows-1251") return 1251;
    return CP_UTF8;
}

void MainWindow::RunProgram(const fs::path& exe, const fs::path& cwd, After mode) {
    bool debug = mode == After::Debug || mode == After::DebugEntry;
    if (settings_->externalConsole && !debug) {
        RunExternal(exe, cwd);
        return;
    }
    Print(std::string("\n") + (debug ? "Отладка " : "Запуск ") + Narrow(exe.filename().wstring()) + "\n", Out::Info);
    if (!StartConsole()) {
        Print("Не удалось создать консоль для программы.\n", Out::Error);
        return;
    }
    UINT codePage = ProgramCodePage();
    Toolchain::SetConsoleCodePage(pty_, codePage);
    std::wstring cmd = Toolchain::QuoteCommandLine({exe.wstring()});

    if (debug) {
        if (!fs::exists(fs::path(exe).replace_extension(L".pdb"))) {
            Print("Нет .pdb рядом с exe — отладка будет без исходников. Для отладки нужны шаблон линковки MSVC "
                  "с /debug и аргументы NASM «-g -F cv8».\n", Out::Warning);
        }
        debugger_ = std::make_unique<Debugger>(hwnd_);
        std::wstring error;
        if (!debugger_->Start(exe, cwd, pty_, CollectBreakpoints(), mode == After::DebugEntry, error)) {
            debugger_.reset();
            Print("Не удалось запустить отладку: " + Narrow(error) + "\n", Out::Error);
            FinishConsole(static_cast<DWORD>(-1));
            return;
        }
        SetDebugging(true);
        SetFocus(console_->hwnd());
        return;
    }

    PROCESS_INFORMATION pi{};
    if (!Toolchain::SpawnInConsole(cmd, cwd, pty_, CREATE_SUSPENDED, pi)) {
        Print("Не удалось запустить программу (код " + std::to_string(GetLastError()) + ")\n", Out::Error);
        FinishConsole(static_cast<DWORD>(-1));
        return;
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    DWORD generation = ++generation_;
    SetFocus(console_->hwnd());
    InvalidateChrome();

    HWND hwnd = hwnd_;
    HANDLE process = process_;
    std::thread([hwnd, process, generation] {
        WaitForSingleObject(process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        PostMessageW(hwnd, WM_APP_PROC_EXIT, code, generation);
    }).detach();
}

void MainWindow::OnProgramExit(DWORD code, DWORD generation) {
    if (generation != generation_ || !process_) return;
    CloseHandle(process_);
    process_ = nullptr;
    if (job_) CloseHandle(job_);
    job_ = nullptr;
    FinishConsole(code);  // reports the exit code once the remaining output is shown
    InvalidateChrome();
}

void MainWindow::StopProgram() {
    if (debugging_ && debugger_) {
        debugger_->Stop();
        Print("\n[отладка остановлена]\n", Out::Warning);
        return;
    }
    if (!process_) return;
    if (job_) TerminateJobObject(job_, 1);
    else TerminateProcess(process_, 1);
    Print("\n[остановлено пользователем]\n", Out::Warning);
}

void MainWindow::RunExternal(const fs::path& exe, const fs::path& cwd) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path bat = fs::path(tmp) / L"asmeditor-run.bat";
    std::wstring script = L"@echo off\r\n";
    if (ProgramCodePage() == CP_UTF8) script += L"chcp 65001 >nul\r\n";
    script += L"title " + exe.filename().wstring() + L"\r\n";
    script += L"cd /d \"" + cwd.wstring() + L"\"\r\n";
    script += L"\"" + exe.wstring() + L"\"\r\n";
    script += L"echo.\r\necho Process exited with code %errorlevel%\r\npause >nul\r\n";
    {
        std::ofstream out(bat, std::ios::binary | std::ios::trunc);
        std::string bytes = Narrow(script, CP_OEMCP);
        out.write(bytes.data(), (std::streamsize)bytes.size());
    }
    std::wstring cmd = L"cmd.exe /c \"" + bat.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Print("\nПрограмма запущена во внешнем окне консоли.\n", Out::Info);
    } else {
        Print("Не удалось открыть консоль.\n", Out::Error);
    }
}

// ---- program console (ConPTY) -----------------------------------------------------------------------------------

std::pair<int, int> MainWindow::ConsoleGridSize() const {
    Editor& c = *console_;
    RECT rc;
    GetClientRect(c.hwnd(), &rc);
    int charW = std::max(1, static_cast<int>(c.Call(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>("M"))));
    int lineH = std::max(1, static_cast<int>(c.Call(SCI_TEXTHEIGHT, 0)));
    int cols = (rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL) - 12) / charW;
    int rows = (rc.bottom - rc.top) / lineH;
    return {std::clamp(cols, 20, 500), std::clamp(rows, 4, 200)};
}

bool MainWindow::StartConsole() {
    EndConsole();
    if (pty_ || ptyReader_.joinable()) FinishConsole(static_cast<DWORD>(-1));  // previous program still closing
    auto [cols, rows] = ConsoleGridSize();
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0)) return false;
    if (!CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return false;
    }
    HPCON pty = nullptr;
    HRESULT hr = CreatePseudoConsole(COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)}, inRead, outWrite, 0, &pty);
    CloseHandle(inRead);  // the pseudoconsole keeps its own duplicates
    CloseHandle(outWrite);
    if (FAILED(hr)) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }
    pty_ = pty;
    ptyInput_ = inWrite;
    term_ = Terminal(cols, rows);
    termStart_ = console_->Call(SCI_GETLENGTH);
    termActive_ = true;
    pendingSurrogate_ = 0;
    DWORD session = ++consoleSession_;
    HWND hwnd = hwnd_;
    ptyReader_ = std::thread([hwnd, outRead, session] {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            PostMessageW(hwnd, WM_APP_PTY_OUTPUT, session, reinterpret_cast<LPARAM>(new std::string(buf, n)));
        }
        CloseHandle(outRead);
    });
    console_->Call(SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
    return true;
}

// Stops mirroring the terminal in the view (the text stays) and stops sending keys to the program.
void MainWindow::EndConsole() {
    if (!termActive_) return;
    RenderTerminal();
    termActive_ = false;
    if (ptyInput_) CloseHandle(ptyInput_);
    ptyInput_ = nullptr;
    console_->Call(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
}

// After the program exits: close the pseudoconsole (this flushes its last output), wait for the reader to drain
// it, then report the exit code. Runs in the background because ClosePseudoConsole can block.
void MainWindow::FinishConsole(DWORD exitCode) {
    HPCON pty = pty_;
    pty_ = nullptr;
    std::thread reader = std::move(ptyReader_);
    HWND hwnd = hwnd_;
    DWORD session = consoleSession_;
    std::thread([pty, reader = std::move(reader), hwnd, session, exitCode]() mutable {
        if (pty) ClosePseudoConsole(pty);
        if (reader.joinable()) reader.join();
        PostMessageW(hwnd, WM_APP_PTY_DONE, exitCode, session);
    }).detach();
}

void MainWindow::OnConsoleDone(DWORD exitCode) {
    EndConsole();
    if (exitCode != static_cast<DWORD>(-1)) {
        Print("\nПроцесс завершён с кодом " + Narrow(Toolchain::DescribeExitCode(exitCode)) + "\n",
              exitCode == 0 ? Out::Info : Out::Error);
    }
    InvalidateChrome();
}

void MainWindow::RenderTerminal() {
    Editor& c = *console_;
    std::vector<TermLine> history = term_.TakeHistory();
    size_t cursor = 0;
    std::vector<TermLine> screen = term_.Screen(cursor);
    c.Call(SCI_SETREADONLY, 0);
    Sci_Position len = c.Call(SCI_GETLENGTH);
    termStart_ = std::min(termStart_, len);
    c.Call(SCI_DELETERANGE, termStart_, len - termStart_);
    auto append = [&](const TermLine& line, bool newline) {
        Sci_Position pos = c.Call(SCI_GETLENGTH);
        std::string text = line.text + (newline ? "\n" : "");
        c.Call(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.data()));
        c.Call(SCI_STARTSTYLING, pos);
        for (auto [bytes, style] : line.runs) c.Call(SCI_SETSTYLING, bytes, style);
        if (newline) c.Call(SCI_SETSTYLING, 1, 0);
    };
    for (const TermLine& line : history) append(line, true);
    termStart_ = c.Call(SCI_GETLENGTH);
    for (size_t i = 0; i < screen.size(); i++) append(screen[i], i + 1 < screen.size());
    c.Call(SCI_SETREADONLY, 1);
    TrimConsole();
    if (termActive_) {
        c.Call(SCI_GOTOPOS, std::min<Sci_Position>(termStart_ + cursor, c.Call(SCI_GETLENGTH)));
        c.Call(SCI_SCROLLCARET);
    }
}

void MainWindow::TrimConsole() {
    Editor& c = *console_;
    Sci_Position len = c.Call(SCI_GETLENGTH);
    if (len <= 4'000'000) return;
    Sci_Position cut = c.Call(SCI_POSITIONFROMLINE, c.Call(SCI_LINEFROMPOSITION, len / 2));
    if (termActive_) cut = std::min(cut, termStart_);
    c.Call(SCI_SETREADONLY, 0);
    c.Call(SCI_DELETERANGE, 0, cut);
    c.Call(SCI_SETREADONLY, 1);
    termStart_ = std::max<Sci_Position>(0, termStart_ - cut);
}

void MainWindow::SendToProgram(const std::string& bytes) {
    if (!ptyInput_ || bytes.empty()) return;
    DWORD written = 0;
    WriteFile(ptyInput_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

// Keyboard input typed into the console view goes to the program as terminal (VT) input.
bool MainWindow::ConsoleKey(UINT msg, WPARAM wp) {
    if (!termActive_ || !ptyInput_) return false;
    Editor& c = *console_;
    if (msg == WM_KEYDOWN) {
        const char* seq = nullptr;
        switch (wp) {
        case VK_UP: seq = "\x1b[A"; break;
        case VK_DOWN: seq = "\x1b[B"; break;
        case VK_RIGHT: seq = "\x1b[C"; break;
        case VK_LEFT: seq = "\x1b[D"; break;
        case VK_HOME: seq = "\x1b[H"; break;
        case VK_END: seq = "\x1b[F"; break;
        case VK_INSERT: seq = "\x1b[2~"; break;
        case VK_DELETE: seq = "\x1b[3~"; break;
        case VK_PRIOR: seq = "\x1b[5~"; break;
        case VK_NEXT: seq = "\x1b[6~"; break;
        default: return false;  // characters arrive as WM_CHAR
        }
        SendToProgram(seq);
        return true;
    }
    // WM_CHAR
    wchar_t ch = static_cast<wchar_t>(wp);
    if (ch == 3 && c.Call(SCI_GETSELECTIONEMPTY) == 0) {  // Ctrl+C with a selection copies instead of interrupting
        c.Call(SCI_COPY);
        return true;
    }
    if (ch == 0x16) {  // Ctrl+V: paste clipboard text as typed input
        std::wstring text;
        if (OpenClipboard(hwnd_)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    text = p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        for (size_t pos; (pos = text.find(L"\r\n")) != std::wstring::npos;) text.erase(pos + 1, 1);
        for (wchar_t& w : text) {
            if (w == L'\n') w = L'\r';
        }
        SendToProgram(Narrow(text));
        return true;
    }
    if (ch == 8) {
        SendToProgram("\x7f");
        return true;
    }
    if (ch >= 0xD800 && ch <= 0xDBFF) {  // first half of a surrogate pair
        pendingSurrogate_ = ch;
        return true;
    }
    std::wstring w;
    if (ch >= 0xDC00 && ch <= 0xDFFF && pendingSurrogate_) w += pendingSurrogate_;
    pendingSurrogate_ = 0;
    w += ch;
    SendToProgram(Narrow(w));  // Enter arrives as '\r', Ctrl+letters as control codes
    return true;
}

LRESULT CALLBACK MainWindow::ConsoleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<MainWindow*>(data);
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && self->ConsoleKey(msg, wp)) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void MainWindow::Print(const std::string& utf8, Out style) {
    Editor& c = *console_;
    if (termActive_) {  // put the program's output so far above the message
        term_.Commit();
        RenderTerminal();
    }
    TrimConsole();
    c.Call(SCI_SETREADONLY, 0);
    Sci_Position len = c.Call(SCI_GETLENGTH);
    std::string text = utf8;
    // Normalise CRLF so the console does not show doubled line breaks.
    for (size_t p; (p = text.find("\r\n")) != std::string::npos;) text.erase(p, 1);
    c.Call(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.data()));
    c.Call(SCI_STARTSTYLING, len);
    c.Call(SCI_SETSTYLING, text.size(), static_cast<int>(style));
    c.Call(SCI_SETREADONLY, 1);
    if (termActive_) termStart_ = c.Call(SCI_GETLENGTH);
    c.Call(SCI_GOTOPOS, c.Call(SCI_GETLENGTH));
}

void MainWindow::ConsoleDoubleClick(Sci_Position pos) {
    Editor& c = *console_;
    Sci_Position line = c.Call(SCI_LINEFROMPOSITION, pos);
    Sci_Position start = c.Call(SCI_POSITIONFROMLINE, line), end = c.Call(SCI_GETLINEENDPOSITION, line);
    std::string text(reinterpret_cast<const char*>(c.Call(SCI_GETRANGEPOINTER, start, end - start)), end - start);
    std::wsmatch m;
    std::wstring w = Widen(text);
    if (!std::regex_search(w, m, std::wregex(L"^\\s*(.+?):(\\d+):"))) return;
    fs::path p = m[1].str();
    if (p.is_relative() && !projectRoot_.empty()) p = projectRoot_ / p;
    if (FileExists(p.wstring())) OpenFile(p, _wtoi(m[2].str().c_str()));
}

void MainWindow::RequestCheck(Editor* ed) {
    if (!ed || !settings_->liveCheck || !Toolchain::NasmAvailable(*settings_)) return;
    CheckJob job{ed->id(), ed->version(), ed->Text(), IncludeDirs(ed->file()), *settings_};
    {
        std::lock_guard<std::mutex> lock(checkMutex_);
        auto it = std::find_if(checkQueue_.begin(), checkQueue_.end(), [&](const CheckJob& j) { return j.editorId == job.editorId; });
        if (it != checkQueue_.end()) *it = std::move(job);
        else checkQueue_.push_back(std::move(job));
    }
    checkCv_.notify_one();
}

void MainWindow::CheckWorker() {
    for (;;) {
        CheckJob job;
        {
            std::unique_lock<std::mutex> lock(checkMutex_);
            checkCv_.wait(lock, [this] { return checkStop_ || !checkQueue_.empty(); });
            if (checkStop_) return;
            job = std::move(checkQueue_.front());
            checkQueue_.pop_front();
        }
        auto* done = new CheckDone{job.editorId, job.version,
                                   Toolchain::Check(job.settings, job.text, job.editorId, job.includes)};
        if (!PostMessageW(hwnd_, WM_APP_CHECK_DONE, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }
}

void MainWindow::RefreshToolStatus() {
    Settings copy = *settings_;
    HWND hwnd = hwnd_;
    std::thread([copy, hwnd] {
        std::wstring version = Toolchain::NasmVersion(copy);
        bool linker = Toolchain::LinkerAvailable(copy);
        std::wstring text;
        bool warn = true;
        if (version.empty()) text = L"NASM не найден — укажите путь в настройках (Ctrl+Alt+S)";
        else if (!linker) text = L"NASM " + version + L" · линкер не найден — проверьте настройки";
        else {
            text = L"NASM " + version + L" · " + fs::path(copy.linkerPath).filename().wstring();
            warn = false;
        }
        PostMessageW(hwnd, WM_APP_TOOL_STATUS, warn, reinterpret_cast<LPARAM>(new std::wstring(text)));
    }).detach();
}
