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
    Editor* ed = focusRight_ && SplitEditor() ? SplitEditor() : Active();
    if (!ed) {
        ShowMessage(hwnd_, Tr(L"Откройте .asm файл или выберите главный файл проекта "
                              L"(ПКМ по файлу в дереве → «Сделать главным файлом»)."), false);
        return {};
    }
    if (ed->file().empty() && !SaveAs(ed)) return {};
    return ed->file();
}

void MainWindow::StartBuild(After after, bool rebuild) {
    if (building_ || debugging_) return;
    if (!Toolchain::NasmAvailable(*settings_)) {
        ShowMessage(hwnd_, Tr(L"NASM не найден. Укажите путь к nasm.exe в настройках."));
        OnCommand(ID_SETTINGS);
        return;
    }
    fs::path src = BuildTarget();
    if (src.empty()) return;
    if (runConfig_.output != L"exe" && after != After::Nothing) {
        ShowMessage(hwnd_, Tr(L"Результат сборки — библиотека: её нельзя запустить. Тип результата меняется в "
                              L"«Конфигурации запуска»."), false);
        after = After::Nothing;
    }
    StopProgram();
    SaveAll();
    ClearProfile();  // the code may change: old counts would sit on the wrong lines
    bool project = IsUnder(src, projectRoot_);
    fs::path outDir = (project ? projectRoot_ : src.parent_path()) / L"build";
    if (runConfig_.Wsl()) outDir /= L"linux";
    if (runConfig_.Release()) outDir /= L"release";
    ClearPane(Pane::Output);
    ShowPane(Pane::Output);
    building_ = true;
    buildDiagnostics_.clear();
    InvalidateChrome();
    Settings copy = *settings_;
    BuildPlan plan;
    plan.outDir = outDir;
    plan.rebuild = rebuild;
    plan.config = runConfig_;
    std::wstring stem = src.stem().wstring();
    bool msvc = _wcsicmp(fs::path(settings_->linkerPath).filename().c_str(), L"link.exe") == 0;
    if (runConfig_.Wsl()) plan.exe = outDir / (runConfig_.output == L"dll" ? L"lib" + stem + L".so" : stem);
    else if (runConfig_.output == L"dll") plan.exe = outDir / (stem + L".dll");
    else if (runConfig_.output == L"lib") plan.exe = outDir / (msvc ? stem + L".lib" : L"lib" + stem + L".a");
    else plan.exe = outDir / (stem + L".exe");
    if (project) {  // a project: link all its source files into one program
        Project::Sources sources = Project::Collect(projectRoot_, src, excludedFromBuild_);
        plan.sources = sources.files;
        plan.root = projectRoot_;
        plan.includes = {projectRoot_};
        for (const fs::path& other : sources.otherPrograms) {
            Print(Tr(std::string("Пропущен (своя функция main): ")) + Narrow(other.filename().wstring()) + "\n", Out::Info);
        }
    } else {
        plan.sources = {src};
        plan.root = src.parent_path();
    }
    if (runConfig_.Release()) Print(Tr(std::string("Профиль Release: без отладочной информации\n")), Out::Info);
    Log::Write("build " + Narrow(src.wstring()) + " -> " + Narrow(plan.exe.wstring()));
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
    buildDiagnostics_ = r->diagnostics;
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
    RefreshProblems();
    if (!r->ok && !r->diagnostics.empty()) ShowPane(Pane::Problems);
    Log::Write(std::string("build ") + (r->ok ? "ok" : "failed"));
    RefreshTree();
    InvalidateChrome();
    if (!r->ok || after == After::Nothing) return;
    fs::path cwd = RunDirectory(src);
    if (runConfig_.Wsl()) RunInWsl(r->exe, cwd, after != After::Run);
    else RunProgram(r->exe, cwd, after);
}

UINT MainWindow::ProgramCodePage() const {
    if (settings_->programEncoding == L"cp866") return 866;
    if (settings_->programEncoding == L"windows-1251") return 1251;
    return CP_UTF8;
}

// Working folder of the program: the one from the run configuration, else the project (or the file's) folder.
fs::path MainWindow::RunDirectory(const fs::path& src) const {
    fs::path base = IsUnder(src, projectRoot_) ? projectRoot_ : src.parent_path();
    if (runConfig_.workDir.empty()) return base;
    fs::path dir = runConfig_.workDir;
    return NormalizePath(dir.is_absolute() ? dir : base / dir);
}

LaunchOptions MainWindow::MakeLaunch(const fs::path& exe, const fs::path& cwd) const {
    LaunchOptions l;
    l.exe = exe;
    l.args = runConfig_.args;
    l.cwd = cwd;
    if (!Trim(runConfig_.env).empty()) l.environment = Toolchain::EnvironmentBlock(runConfig_.env);
    return l;
}

// The stdin file of the run configuration is typed into the program, followed by Ctrl+Z (end of input).
void MainWindow::FeedStdin() {
    if (Trim(runConfig_.stdinFile).empty()) return;
    fs::path file = runConfig_.stdinFile;
    if (file.is_relative() && !projectRoot_.empty()) file = projectRoot_ / file;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        Print(Tr(std::string("Не найден файл для stdin: ")) + Narrow(file.wstring()) + "\n", Out::Warning);
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str(), typed;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') continue;
        typed += text[i] == '\n' ? '\r' : text[i];
    }
    if (!typed.empty() && typed.back() != '\r') typed += '\r';
    typed += "\x1a\r";
    SendToProgram(typed);
    Print(Tr(std::string("stdin: передано содержимое файла ")) + Narrow(file.filename().wstring()) + "\n", Out::Info);
}

void MainWindow::RunProgram(const fs::path& exe, const fs::path& cwd, After mode) {
    bool debug = mode == After::Debug || mode == After::DebugEntry || mode == After::RunToCursor ||
                 mode == After::Profile;
    if (settings_->externalConsole && !debug) {
        RunExternal(exe, cwd);
        return;
    }
    Print("\n" + Tr(std::string(mode == After::Profile ? "Профилирование " : debug ? "Отладка " : "Запуск ")) +
              Narrow(exe.filename().wstring()) +
              (runConfig_.args.empty() ? "" : " " + Narrow(runConfig_.args)) + "\n", Out::Info);
    ClearPane(Pane::Terminal);
    if (!StartConsole()) {
        Print(Tr(std::string("Не удалось создать консоль для программы.\n")), Out::Error);
        return;
    }
    UINT codePage = ProgramCodePage();
    Toolchain::SetConsoleCodePage(pty_, codePage);
    LaunchOptions launch = MakeLaunch(exe, cwd);
    launch.profile = mode == After::Profile;
    runStartTick_ = GetTickCount64();
    Log::Write("run " + Narrow(exe.wstring()) + (debug ? " (debug)" : ""));

    if (debug) {
        if (!fs::exists(fs::path(exe).replace_extension(L".pdb"))) {
            Print(Tr(std::string("Нет .pdb рядом с exe — отладка будет без исходников. Для отладки нужны шаблон линковки "
                                 "MSVC с /debug, аргументы NASM «-g -F cv8» и профиль Debug.\n")), Out::Warning);
        }
        debugger_ = std::make_unique<Debugger>(hwnd_);
        std::wstring error;
        std::optional<SourceLocation> runTo = mode == After::RunToCursor ? runTo_ : std::nullopt;
        runTo_.reset();
        if (!debugger_->Start(launch, pty_, CollectBreakpoints(), mode == After::DebugEntry, runTo, error)) {
            debugger_.reset();
            Print(Tr(std::string("Не удалось запустить отладку: ")) + Narrow(error) + "\n", Out::Error);
            FinishConsole(static_cast<DWORD>(-1));
            return;
        }
        SetDebugging(true);
        ShowPane(Pane::Terminal);
        SetFocus(View(Pane::Terminal).hwnd());
        FeedStdin();
        return;
    }

    PROCESS_INFORMATION pi{};
    std::wstring cmd = Toolchain::QuoteCommandLine({exe.wstring()});
    if (!launch.args.empty()) cmd += L" " + launch.args;
    if (!Toolchain::SpawnInConsole(cmd, cwd, pty_, CREATE_SUSPENDED, pi, launch.environment)) {
        Print(Tr(std::string("Не удалось запустить программу (код ")) + std::to_string(GetLastError()) + ")\n", Out::Error);
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
    ShowPane(Pane::Terminal);
    SetFocus(View(Pane::Terminal).hwnd());
    InvalidateChrome();
    FeedStdin();

    HWND hwnd = hwnd_;
    HANDLE process = process_;
    std::thread([hwnd, process, generation] {
        WaitForSingleObject(process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        PostMessageW(hwnd, WM_APP_PROC_EXIT, code, generation);
    }).detach();
}

// A Linux program (built with -f elf64) runs inside WSL in the same terminal; "debugging" starts gdb there with
// the editor's breakpoints, and gdb is then used through its own commands.
void MainWindow::RunInWsl(const fs::path& exe, const fs::path& cwd, bool debug) {
    if (!Toolchain::FindOnPath(L"wsl.exe")) {
        Print(Tr(std::string("wsl.exe не найден: для Linux-программ нужна подсистема Windows для Linux (WSL).\n")), Out::Error);
        return;
    }
    Print("\n" + Tr(std::string(debug ? "Отладка в gdb (WSL) " : "Запуск в WSL ")) + Narrow(exe.filename().wstring()) + "\n",
          Out::Info);
    ClearPane(Pane::Terminal);
    if (!StartConsole()) {
        Print(Tr(std::string("Не удалось создать консоль для программы.\n")), Out::Error);
        return;
    }
    std::vector<std::wstring> cmd{L"wsl.exe", L"--cd", Toolchain::WslPath(cwd), L"--exec"};
    if (debug) {
        cmd.insert(cmd.end(), {L"gdb", L"-q"});
        static const std::regex reg("\\b(r[a-ds][xip]|r[sd]i|r[sb]p|r(?:8|9|1[0-5])[dwb]?|e[a-ds][xip]|e[sd]i|e[sb]p|rip|eflags)\\b");
        for (const auto& [key, lines] : CollectBreakpoints()) {
            for (const auto& [line, condition] : lines) {
                std::string where = Narrow(fs::path(key).filename().wstring()) + ":" + std::to_string(line);
                if (!condition.empty() && condition[0] != '#') {
                    where += " if " + std::regex_replace(condition, reg, "$$$1");  // gdb names registers $rax
                }
                cmd.push_back(L"-ex");
                cmd.push_back(L"break " + Widen(where));
            }
        }
        cmd.push_back(L"-ex");
        cmd.push_back(CollectBreakpoints().empty() ? L"starti" : L"run");
        cmd.push_back(L"--args");
        Print(Tr(std::string("Команды gdb: next, step, continue, info registers, x/8xg $rsp, quit\n")), Out::Info);
    }
    cmd.push_back(Toolchain::WslPath(exe));
    for (const std::wstring& a : Toolchain::SplitArgs(runConfig_.args)) cmd.push_back(a);

    PROCESS_INFORMATION pi{};
    if (!Toolchain::SpawnInConsole(Toolchain::QuoteCommandLine(cmd), cwd, pty_, CREATE_SUSPENDED, pi)) {
        Print(Tr(std::string("Не удалось запустить программу (код ")) + std::to_string(GetLastError()) + ")\n", Out::Error);
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
    runStartTick_ = GetTickCount64();
    DWORD generation = ++generation_;
    ShowPane(Pane::Terminal);
    SetFocus(View(Pane::Terminal).hwnd());
    InvalidateChrome();
    if (!debug) FeedStdin();
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
    exitStats_ = Toolchain::ProcessStats(process_, runStartTick_);
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
        Print("\n" + Tr(std::string("[отладка остановлена]")) + "\n", Out::Warning);
        return;
    }
    if (!process_) return;
    if (job_) TerminateJobObject(job_, 1);
    else TerminateProcess(process_, 1);
    Print("\n" + Tr(std::string("[остановлено пользователем]")) + "\n", Out::Warning);
}

void MainWindow::RunExternal(const fs::path& exe, const fs::path& cwd) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path bat = fs::path(tmp) / L"asmeditor-run.bat";
    std::wstring script = L"@echo off\r\n";
    if (ProgramCodePage() == CP_UTF8) script += L"chcp 65001 >nul\r\n";
    script += L"title " + exe.filename().wstring() + L"\r\n";
    script += L"cd /d \"" + cwd.wstring() + L"\"\r\n";
    for (const std::wstring& line : Split(runConfig_.env, L'\n')) script += L"set \"" + line + L"\"\r\n";
    script += L"\"" + exe.wstring() + L"\" " + runConfig_.args;
    if (!Trim(runConfig_.stdinFile).empty()) {
        fs::path in = runConfig_.stdinFile;
        if (in.is_relative() && !projectRoot_.empty()) in = projectRoot_ / in;
        script += L" < \"" + in.wstring() + L"\"";
    }
    script += L"\r\necho.\r\necho Process exited with code %errorlevel%\r\npause >nul\r\n";
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
        Print("\n" + Tr(std::string("Программа запущена во внешнем окне консоли.")) + "\n", Out::Info);
    } else {
        Print(Tr(std::string("Не удалось открыть консоль.\n")), Out::Error);
    }
}

// ---- run configuration, target and profile ------------------------------------------------------------------------

void MainWindow::EditRunConfig() {
    if (projectRoot_.empty()) {
        ShowMessage(hwnd_, Tr(L"Конфигурация запуска хранится в проекте: откройте папку проекта (Ctrl+Shift+O)."), false);
        return;
    }
    if (!ShowRunConfigDialog(hwnd_, runConfig_)) return;
    SaveProjectSession();
    for (auto& e : editors_) RequestCheck(e.get());  // Linux / Windows changes the object format
    Layout();
}

void MainWindow::ChooseTarget() {
    if (debugging_ || building_) return;
    std::vector<fs::path> programs = ProgramFiles();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (mainFile_.empty() ? MF_CHECKED : 0), 1, Tr(L"Активный файл"));
    if (!programs.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < programs.size(); i++) {
        fs::path rel = NormalizePath(programs[i]).lexically_relative(NormalizePath(projectRoot_));
        AppendMenuW(menu, MF_STRING | (SamePath(programs[i], mainFile_) ? MF_CHECKED : 0), 100 + i, rel.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, MenuText(ID_RUN_CONFIG).c_str());
    RECT r{};
    for (const ToolItem& t : tools_) {
        if (t.id == ID_TARGET) r = t.rc;
    }
    POINT p{r.left, r.bottom};
    ClientToScreen(hwnd_, &p);
    int cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) SetMainFile({});
    else if (cmd == 2) EditRunConfig();
    else if (cmd >= 100 && cmd - 100 < static_cast<int>(programs.size())) SetMainFile(programs[cmd - 100]);
}

void MainWindow::ChooseProfile() {
    if (debugging_ || building_) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (!runConfig_.Release() ? MF_CHECKED : 0), ID_PROFILE_DEBUG,
                Tr(L"Debug — с отладочной информацией"));
    AppendMenuW(menu, MF_STRING | (runConfig_.Release() ? MF_CHECKED : 0), ID_PROFILE_RELEASE,
                Tr(L"Release — без отладочной информации"));
    RECT r{};
    for (const ToolItem& t : tools_) {
        if (t.id == ID_PROFILE) r = t.rc;
    }
    POINT p{r.left, r.bottom};
    ClientToScreen(hwnd_, &p);
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, hwnd_, nullptr);
    DestroyMenu(menu);
}

// ---- problems -----------------------------------------------------------------------------------------------------

// Every error and warning: from the last build and from the live check of the open files.
void MainWindow::RefreshProblems() {
    Editor& v = View(Pane::Problems);
    Sci_Position first = v.Call(SCI_GETFIRSTVISIBLELINE);
    ClearPane(Pane::Problems);
    auto shown = [&](const fs::path& f) {
        if (!projectRoot_.empty() && IsUnder(f, projectRoot_)) return NormalizePath(f).lexically_relative(NormalizePath(projectRoot_)).wstring();
        return f.wstring();
    };
    std::set<std::pair<std::wstring, int>> seen;
    int count = 0;
    auto add = [&](const std::wstring& file, const Diagnostic& d) {
        if (!seen.insert({Lower(file) + L"|" + d.message, d.line}).second) return;
        bool error = d.severity == Severity::Error;
        PrintTo(Pane::Problems, Narrow(file) + ":" + std::to_string(d.line) + ": ", Out::Input);
        PrintTo(Pane::Problems, Tr(std::string(error ? "ошибка" : "предупреждение")) + ": ", error ? Out::Error : Out::Warning);
        PrintTo(Pane::Problems, Narrow(d.message) + "\n", Out::Normal);
        count++;
    };
    for (const Diagnostic& d : buildDiagnostics_) add(shown(d.file), d);
    for (auto& e : editors_) {
        std::wstring name = e->file().empty() ? e->Title() : shown(e->file());
        for (const auto& [line, diags] : e->Diagnostics()) {
            for (const Diagnostic& d : diags) add(name, d);
        }
    }
    problemCount_ = count;
    if (count == 0) PrintTo(Pane::Problems, Tr(std::string("Проблем нет.\n")), Out::Success);
    else PrintTo(Pane::Problems, "\n" + Tr(std::string("Двойной клик открывает место ошибки.")) + "\n", Out::Info);
    v.Call(SCI_SETFIRSTVISIBLELINE, first);
    InvalidateRect(hwnd_, &rcConsoleHeader_, FALSE);
}

// ---- exercises ----------------------------------------------------------------------------------------------------

namespace {

// Runs a program with the given standard input; stdout and stderr together. Kills it after `timeoutMs`.
bool RunWithInput(const fs::path& exe, const std::string& input, DWORD timeoutMs, std::string& output, DWORD& code) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead, inWrite, outRead, outWrite;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return false;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return false;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi{};
    bool started = Toolchain::Spawn(Toolchain::QuoteCommandLine({exe.wstring()}), exe.parent_path(), inRead, outWrite,
                                    outWrite, CREATE_NO_WINDOW, pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!started) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }
    std::thread writer([inWrite, input] {
        DWORD n = 0;
        std::string data = input + "\n";
        WriteFile(inWrite, data.data(), static_cast<DWORD>(data.size()), &n, nullptr);
        CloseHandle(inWrite);
    });
    std::thread reader([outRead, &output] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0 && output.size() < 1'000'000) output.append(buf, n);
        CloseHandle(outRead);
    });
    bool finished = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    if (!finished) TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 2000);
    GetExitCodeProcess(pi.hProcess, &code);
    writer.join();
    reader.join();
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return finished;
}

std::string OneLine(std::string s) {
    for (char& c : s) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() > 80) s = s.substr(0, 80) + "…";
    return s;
}
}

// Builds the exercise in the current file and runs it on every test of the exercise.
void MainWindow::CheckSolution() {
    Editor* ed = focusRight_ && SplitEditor() ? SplitEditor() : Active();
    if (!ed || building_ || debugging_) return;
    std::string id = Exercises::IdFromSource(ed->Text());
    const Exercises::Exercise* ex = Exercises::Find(id);
    if (!ex) {
        ShowMessage(hwnd_, Tr(L"В начале файла нет строки «; task: …» с названием задачи. Создайте задачу: "
                              L"Запуск → «Новая учебная задача»."), false);
        return;
    }
    if (!Toolchain::NasmAvailable(*settings_)) {
        ShowMessage(hwnd_, Tr(L"NASM не найден. Укажите путь к nasm.exe в настройках."));
        return;
    }
    if (ed->file().empty() && !SaveAs(ed)) return;
    Save(ed);
    StopProgram();
    ClearPane(Pane::Output);
    ShowPane(Pane::Output);
    Print(Tr(std::string("Проверка задачи ")) + "«" + Narrow(Tr(ex->title.c_str())) + "»\n", Out::Info);
    building_ = true;
    InvalidateChrome();
    BuildPlan plan;
    plan.sources = {ed->file()};
    plan.root = ed->file().parent_path();
    plan.outDir = plan.root / L"build";
    plan.exe = plan.outDir / (ed->file().stem().wstring() + L".exe");
    plan.includes = IncludeDirs(ed->file());
    Settings copy = *settings_;
    HWND hwnd = hwnd_;
    Exercises::Exercise exercise = *ex;
    std::thread([copy, plan, hwnd, exercise] {
        auto post = [hwnd](const std::string& text, Out style) {
            PostMessageW(hwnd, WM_APP_OUTPUT, static_cast<WPARAM>(style), reinterpret_cast<LPARAM>(new std::string(text)));
        };
        BuildResult built = Toolchain::Build(copy, plan, post);
        if (!built.ok) {
            PostMessageW(hwnd, WM_APP_SOLUTION_DONE, 0,
                         reinterpret_cast<LPARAM>(new std::string(Tr(std::string("Решение не собирается — исправьте ошибки.\n")))));
            return;
        }
        int passed = 0;
        for (size_t i = 0; i < exercise.tests.size(); i++) {
            const Exercises::Test& t = exercise.tests[i];
            std::string output;
            DWORD code = 0;
            bool finished = RunWithInput(built.exe, t.input, 5000, output, code);
            bool ok = finished && Exercises::OutputMatches(output, t.expected);
            std::string line = Tr(std::string("Тест")) + " " + std::to_string(i + 1) + ": ";
            if (ok) {
                passed++;
                post(line + "✔  " + Tr(std::string("ввод")) + " «" + OneLine(t.input) + "» → «" + OneLine(t.expected) + "»\n",
                     Out::Success);
            } else {
                std::string why = !finished ? Tr(std::string("программа не завершилась за 5 секунд"))
                                            : Tr(std::string("получено")) + " «" + OneLine(output) + "»" +
                                                  (code ? " (" + Tr(std::string("код")) + " " +
                                                              Narrow(Toolchain::DescribeExitCode(code)) + ")" : "");
                post(line + "✘  " + Tr(std::string("ввод")) + " «" + OneLine(t.input) + "», " + Tr(std::string("ожидалось")) +
                         " «" + OneLine(t.expected) + "», " + why + "\n", Out::Error);
            }
        }
        bool all = passed == static_cast<int>(exercise.tests.size());
        std::string summary = all ? Tr(std::string("Все тесты пройдены — задача решена!"))
                                  : Tr(std::string("Пройдено тестов:")) + " " + std::to_string(passed) + " / " +
                                        std::to_string(exercise.tests.size());
        PostMessageW(hwnd, WM_APP_SOLUTION_DONE, all ? 1 : 0, reinterpret_cast<LPARAM>(new std::string(summary + "\n")));
    }).detach();
}

void MainWindow::NewExercise() {
    std::vector<std::wstring> items;
    for (const Exercises::Exercise& e : Exercises::All()) {
        std::string first = e.description.substr(0, e.description.find('\n'));
        items.push_back(std::wstring(Tr(e.title.c_str())) + L"\t" + Widen(first));
    }
    auto pick = PickFromList(hwnd_, Tr(L"Учебные задачи"), items, Tr(L"Название задачи…"));
    if (!pick) return;
    const Exercises::Exercise& e = Exercises::All()[*pick];
    fs::path dir;
    if (!projectRoot_.empty()) {
        dir = projectRoot_ / L"tasks";
    } else if (auto folder = PickFolder(Tr(L"Папка для учебных задач"))) {
        dir = *folder;
    } else {
        return;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file = dir / (Widen(e.id) + L".asm");
    if (!fs::exists(file)) {
        std::ofstream out(file, std::ios::binary);
        out << e.starter;
    }
    // Each exercise is its own program: keep it out of the project's build.
    if (!projectRoot_.empty()) excludedFromBuild_.insert(Project::Key(file));
    SaveProjectSession();
    RefreshTree();
    OpenFile(file);
    ClearPane(Pane::Output);
    ShowPane(Pane::Output);
    Print(Narrow(Tr(e.title.c_str())) + "\n" + e.description + "\n\n" +
              Tr(std::string("Напишите решение и нажмите Ctrl+Shift+T — редактор соберёт программу и проверит её на тестах.")) +
              "\n", Out::Info);
}

// ---- program console (ConPTY) -----------------------------------------------------------------------------------

std::pair<int, int> MainWindow::ConsoleGridSize() const {
    Editor& c = View(Pane::Terminal);
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
    termStart_ = View(Pane::Terminal).Call(SCI_GETLENGTH);
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
    View(Pane::Terminal).Call(SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
    return true;
}

// Stops mirroring the terminal in the view (the text stays) and stops sending keys to the program.
void MainWindow::EndConsole() {
    if (!termActive_) return;
    RenderTerminal();
    termActive_ = false;
    if (ptyInput_) CloseHandle(ptyInput_);
    ptyInput_ = nullptr;
    View(Pane::Terminal).Call(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
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
        std::string text = "\n" + Tr(std::string("Процесс завершён с кодом ")) + Narrow(Toolchain::DescribeExitCode(exitCode));
        if (!exitStats_.empty()) text += " · " + exitStats_;
        Out style = exitCode == 0 ? Out::Info : Out::Error;
        Print(text + "\n", style);
        PrintTo(Pane::Terminal, text + "\n", style);
        exitStats_.clear();
    }
    InvalidateChrome();
}

void MainWindow::RenderTerminal() {
    Editor& c = View(Pane::Terminal);
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

// Keeps the terminal view below a few megabytes (the screen part is never cut).
void MainWindow::TrimConsole() {
    Editor& c = View(Pane::Terminal);
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
    Editor& c = View(Pane::Terminal);
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
    PrintTo(Pane::Output, utf8, style);
}

void MainWindow::PrintTo(Pane pane, const std::string& utf8, Out style) {
    Editor& c = View(pane);
    if (pane == Pane::Terminal && termActive_) {  // only while no program writes there
        term_.Commit();
        RenderTerminal();
    }
    c.Call(SCI_SETREADONLY, 0);
    Sci_Position len = c.Call(SCI_GETLENGTH);
    if (len > 4'000'000) {
        Sci_Position cut = c.Call(SCI_POSITIONFROMLINE, c.Call(SCI_LINEFROMPOSITION, len / 2));
        c.Call(SCI_DELETERANGE, 0, cut);
        if (pane == Pane::Terminal) termStart_ = std::max<Sci_Position>(0, termStart_ - cut);
        len = c.Call(SCI_GETLENGTH);
    }
    std::string text = utf8;
    // Normalise CRLF so the console does not show doubled line breaks.
    for (size_t p; (p = text.find("\r\n")) != std::string::npos;) text.erase(p, 1);
    c.Call(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.data()));
    c.Call(SCI_STARTSTYLING, len);
    c.Call(SCI_SETSTYLING, text.size(), static_cast<int>(style));
    c.Call(SCI_SETREADONLY, 1);
    if (pane == Pane::Terminal && termActive_) termStart_ = c.Call(SCI_GETLENGTH);
    c.Call(SCI_GOTOPOS, c.Call(SCI_GETLENGTH));
}

void MainWindow::ClearPane(Pane pane) {
    Editor& c = View(pane);
    c.Call(SCI_SETREADONLY, 0);
    c.Call(SCI_CLEARALL);
    c.Call(SCI_SETREADONLY, 1);
    if (pane == Pane::Terminal) {
        termStart_ = 0;
        if (termActive_) {
            term_.Commit();
            term_.TakeHistory();
            RenderTerminal();
        }
    }
}

void MainWindow::ShowPane(Pane pane) {
    pane_ = pane;
    for (int i = 0; i < static_cast<int>(Pane::Count); i++) {
        ShowWindow(views_[i]->hwnd(), i == static_cast<int>(pane) ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(hwnd_, &rcConsoleHeader_, FALSE);
}

// "file:line: ..." in the output, problems and search lists opens the file at that line. A file name that is not
// on disk may be the title of an unsaved tab.
void MainWindow::ConsoleDoubleClick(Editor& c, Sci_Position pos) {
    Sci_Position line = c.Call(SCI_LINEFROMPOSITION, pos);
    Sci_Position start = c.Call(SCI_POSITIONFROMLINE, line), end = c.Call(SCI_GETLINEENDPOSITION, line);
    std::string text(reinterpret_cast<const char*>(c.Call(SCI_GETRANGEPOINTER, start, end - start)), end - start);
    std::wsmatch m;
    std::wstring w = Widen(text);
    if (!std::regex_search(w, m, std::wregex(L"^\\s*(.+?):(\\d+):"))) return;
    int lineNo = _wtoi(m[2].str().c_str());
    fs::path p = m[1].str();
    if (p.is_relative() && !projectRoot_.empty()) p = projectRoot_ / p;
    if (FileExists(p.wstring())) {
        OpenFile(p, lineNo);
        return;
    }
    for (size_t i = 0; i < editors_.size(); i++) {
        if (editors_[i]->file().empty() && editors_[i]->Title() == m[1].str()) {
            Activate(static_cast<int>(i));
            editors_[i]->GotoLine(lineNo);
        }
    }
}

void MainWindow::RequestCheck(Editor* ed) {
    if (!ed || !settings_->liveCheck || !Toolchain::NasmAvailable(*settings_)) return;
    CheckJob job{ed->id(), ed->version(), ed->Text(), IncludeDirs(ed->file()), *settings_, runConfig_};
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
                                   Toolchain::Check(job.settings, job.config, job.text, job.editorId, job.includes)};
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
