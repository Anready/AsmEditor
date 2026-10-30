#include "debugger.h"

#include <dbghelp.h>

namespace {

constexpr DWORD kTrapFlag = 0x100;
constexpr DWORD kStatusWx86Breakpoint = 0x4000001F;

struct SymbolBuffer {
    SYMBOL_INFOW info;
    wchar_t name[MAX_SYM_NAME];
    SymbolBuffer() {
        ZeroMemory(this, sizeof(*this));
        info.SizeOfStruct = sizeof(SYMBOL_INFOW);
        info.MaxNameLen = MAX_SYM_NAME;
    }
};

std::string LowerAscii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

void Log(HWND hwnd, const std::string& text, Out style) {
    PostMessageW(hwnd, WM_DEBUG_LOG, static_cast<WPARAM>(style), reinterpret_cast<LPARAM>(new std::string(text)));
}

// Debug-event trace for diagnosing the debugger itself (set ASMEDITOR_TRACE=1).
void Trace(HWND hwnd, const char* what, uint64_t a = 0, uint64_t b = 0) {
    static const bool enabled = [] {
        size_t n = 0;
        return getenv_s(&n, nullptr, 0, "ASMEDITOR_TRACE") == 0 && n > 0;
    }();
    if (!enabled) return;
    char buf[160];
    sprintf_s(buf, "[trace %lu] %s %llX %llX\n", GetTickCount() % 100000, what, static_cast<unsigned long long>(a),
              static_cast<unsigned long long>(b));
    Log(hwnd, buf, Out::Info);
}

// Context changes are only made on the thread that reported the debug event: on another thread they can block
// for seconds (see Pause()).
bool GetContext(HANDLE thread, CONTEXT& ctx) {
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_ALL;
    return GetThreadContext(thread, &ctx) != FALSE;
}

bool SetContext(HANDLE thread, const CONTEXT& ctx) {
    alignas(16) CONTEXT control = ctx;
    control.ContextFlags = CONTEXT_CONTROL;  // only rip/rsp/eflags are ever changed
    return SetThreadContext(thread, &control) != FALSE;
}

// Small expression language for the memory view and hover tips:
//   numbers (42, 0x2A, 2Ah, 0b101), registers, symbols, + - *, (expr), [expr] = qword at address.
class ExpressionParser {
public:
    ExpressionParser(Debugger& d, const std::string& s, std::wstring& err) : d_(d), s_(s), err_(err) {}

    std::optional<uint64_t> Parse() {
        auto v = Expr();
        Skip();
        if (v && i_ < s_.size()) {
            err_ = L"лишние символы: " + Widen(s_.substr(i_));
            return std::nullopt;
        }
        return v;
    }

private:
    Debugger& d_;
    const std::string& s_;
    std::wstring& err_;
    size_t i_ = 0;

    void Skip() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) i_++;
    }
    bool Eat(char c) {
        Skip();
        if (i_ < s_.size() && s_[i_] == c) {
            i_++;
            return true;
        }
        return false;
    }
    std::optional<uint64_t> Expr() {
        auto v = Term();
        while (v) {
            bool plus = Eat('+');
            if (!plus && !Eat('-')) break;
            auto r = Term();
            if (!r) return std::nullopt;
            *v = plus ? *v + *r : *v - *r;
        }
        return v;
    }
    std::optional<uint64_t> Term() {
        auto v = Factor();
        while (v && Eat('*')) {
            auto r = Factor();
            if (!r) return std::nullopt;
            *v *= *r;
        }
        return v;
    }
    std::optional<uint64_t> Factor() {
        Skip();
        if (i_ >= s_.size()) {
            err_ = L"ожидалось выражение";
            return std::nullopt;
        }
        if (Eat('-')) {
            auto v = Factor();
            if (v) *v = 0 - *v;
            return v;
        }
        if (Eat('(')) {
            auto v = Expr();
            if (v && !Eat(')')) {
                err_ = L"ожидалась «)»";
                return std::nullopt;
            }
            return v;
        }
        if (Eat('[')) {
            auto v = Expr();
            if (!v) return std::nullopt;
            if (!Eat(']')) {
                err_ = L"ожидалась «]»";
                return std::nullopt;
            }
            uint64_t q = 0;
            if (d_.ReadMemory(*v, &q, 8) != 8) {
                err_ = L"память по адресу недоступна";
                return std::nullopt;
            }
            return q;
        }
        size_t start = i_;
        auto isWord = [](char c) {
            return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' ||
                   static_cast<unsigned char>(c) >= 0x80;
        };
        while (i_ < s_.size() && isWord(s_[i_])) i_++;
        std::string word = s_.substr(start, i_ - start);
        if (word.empty()) {
            err_ = L"неожиданный символ «" + Widen(std::string(1, s_[i_])) + L"»";
            return std::nullopt;
        }
        if (isdigit(static_cast<unsigned char>(word[0]))) return Number(word);
        if (auto r = Debugger::Register(d_.Context(), LowerAscii(word))) return r;
        if (auto a = d_.LookupSymbol(word)) return a;
        err_ = L"неизвестное имя «" + Widen(word) + L"»";
        return std::nullopt;
    }
    std::optional<uint64_t> Number(const std::string& w) {
        std::string t = LowerAscii(w);
        t.erase(std::remove(t.begin(), t.end(), '_'), t.end());
        int base = 10;
        if (t.rfind("0x", 0) == 0) {
            base = 16;
            t = t.substr(2);
        } else if (t.rfind("0b", 0) == 0 && t.size() > 2 && t.find_first_not_of("01", 2) == std::string::npos) {
            base = 2;
            t = t.substr(2);
        } else if (!t.empty() && t.back() == 'h') {
            base = 16;
            t.pop_back();
        }
        char* end = nullptr;
        uint64_t v = t.empty() ? 0 : strtoull(t.c_str(), &end, base);
        if (t.empty() || *end) {
            err_ = L"неверное число «" + Widen(w) + L"»";
            return std::nullopt;
        }
        return v;
    }
};
}

std::optional<uint64_t> Debugger::Register(const CONTEXT& c, const std::string& n) {
    const DWORD64* r[] = {&c.Rax, &c.Rbx, &c.Rcx, &c.Rdx, &c.Rsi, &c.Rdi, &c.Rbp, &c.Rsp,
                          &c.R8, &c.R9, &c.R10, &c.R11, &c.R12, &c.R13, &c.R14, &c.R15};
    static const char* n64[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    static const char* n32[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
                                "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
    static const char* n16[] = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
                                "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
    static const char* n8[] = {"al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl",
                               "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
    for (int i = 0; i < 16; i++) {
        if (n == n64[i]) return *r[i];
        if (n == n32[i]) return *r[i] & 0xFFFFFFFFull;
        if (n == n16[i]) return *r[i] & 0xFFFFull;
        if (n == n8[i] || (i >= 8 && n == std::string(n64[i]) + "l")) return *r[i] & 0xFFull;
    }
    if (n == "ah") return (c.Rax >> 8) & 0xFF;
    if (n == "bh") return (c.Rbx >> 8) & 0xFF;
    if (n == "ch") return (c.Rcx >> 8) & 0xFF;
    if (n == "dh") return (c.Rdx >> 8) & 0xFF;
    if (n == "rip") return c.Rip;
    if (n == "eip") return c.Rip & 0xFFFFFFFF;
    if (n == "rflags" || n == "eflags") return c.EFlags;
    return std::nullopt;
}

Debugger::Debugger(HWND notify) : notify_(notify) {}

Debugger::~Debugger() {
    Stop();
    if (thread_.joinable()) thread_.join();
}

// ---- commands (UI thread) ---------------------------------------------------------------------------------------

bool Debugger::Start(const fs::path& exe, const fs::path& cwd, HPCON console,
                     const Breakpoints& breakpoints, bool stopAtEntry, std::wstring& error) {
    std::promise<std::wstring> started;
    auto result = started.get_future();
    running_ = true;
    thread_ = std::thread(&Debugger::Loop, this, exe, cwd, console, breakpoints, stopAtEntry, &started);
    error = result.get();  // waits until the process is created (or failed)
    if (!error.empty()) {
        thread_.join();
        running_ = false;
        return false;
    }
    return true;
}

void Debugger::Send(Command c) {
    std::lock_guard<std::mutex> lock(commandMutex_);
    command_ = c;
    cv_.notify_all();
}

void Debugger::Pause() {
    HANDLE p = process_;
    if (!running_ || stopped_ || !p || pauseRequested_) return;
    pauseRequested_ = true;
    {
        // Put temporary int3 on every source line of the program: the main thread stops at the next line it
        // reaches and reports the event itself. (Touching another thread's context is very slow on Windows.)
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (lineAddresses_.empty()) {
            SymEnumSourceLinesW(p, imageBase_, nullptr, nullptr, 0, 0, [](PSRCCODEINFOW info, PVOID self) -> BOOL {
                auto* d = static_cast<Debugger*>(self);
                if (FileExists(info->FileName)) d->lineAddresses_.push_back(info->Address);
                return TRUE;
            }, this);
        }
        for (uint64_t a : lineAddresses_) Install(a, false, false, true);
    }
    // A thread blocked in the kernel (e.g. waiting for input) never reaches a line: break in after a moment.
    std::thread([this, p] {
        Sleep(1000);
        if (pauseRequested_ && running_ && !stopped_) DebugBreakProcess(p);
    }).detach();
}

void Debugger::Stop() {
    Send(Command::Stop);
    HANDLE p = process_;
    if (running_ && p) TerminateProcess(p, 1);
}

void Debugger::SetBreakpoints(const fs::path& file, const std::set<int>& lines) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!process_) return;
    std::wstring key = Key(file);
    RemoveUserFile(key);
    InstallFile(key, lines);
}

// ---- debug loop (debugger thread) -------------------------------------------------------------------------------

void Debugger::Loop(fs::path exe, fs::path cwd, HPCON console, Breakpoints bps, bool stopAtEntry,
                    std::promise<std::wstring>* started) {
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Toolchain::QuoteCommandLine({exe.wstring()});
    if (!Toolchain::SpawnInConsole(cmd, cwd, console, DEBUG_ONLY_THIS_PROCESS, pi)) {
        started->set_value(L"не удалось запустить процесс (код " + std::to_wstring(GetLastError()) + L")");
        return;
    }
    CloseHandle(pi.hProcess);  // the debug events provide their own handles
    CloseHandle(pi.hThread);
    started->set_value(L"");

    DWORD exitCode = 0;
    for (bool done = false; !done;) {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEventEx(&ev, 100)) continue;
        DWORD status = DBG_CONTINUE;
        Trace(notify_, "event", ev.dwDebugEventCode * 0x100000000ull + ev.dwThreadId,
              ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT ? ev.u.Exception.ExceptionRecord.ExceptionCode : 0);
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT: {
            const auto& info = ev.u.CreateProcessInfo;
            process_ = info.hProcess;
            mainThread_ = info.hThread;
            threads_[ev.dwThreadId] = info.hThread;
            imageBase_ = reinterpret_cast<uint64_t>(info.lpBaseOfImage);
            imageSize_ = ImageSize(imageBase_);
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
                SymInitializeW(process_, exe.parent_path().c_str(), FALSE);
                if (!SymLoadModuleExW(process_, info.hFile, exe.c_str(), nullptr, imageBase_, 0, nullptr, 0)) {
                    Log(notify_, "Не удалось загрузить отладочную информацию\n", Out::Warning);
                }
                IMAGEHLP_MODULEW64 mod{sizeof(mod)};
                if (SymGetModuleInfoW64(process_, imageBase_, &mod) && !mod.LineNumbers) {
                    Log(notify_, "В программе нет номеров строк: нужны «-g -F cv8» для NASM и /debug для link.\n",
                        Out::Warning);
                }
                for (const auto& [key, lines] : bps) InstallFile(key, lines);
                if (stopAtEntry) {
                    SymbolBuffer sym;
                    uint64_t entry = SymFromNameW(process_, L"main", &sym.info)
                                         ? sym.info.Address
                                         : reinterpret_cast<uint64_t>(info.lpStartAddress);
                    Install(entry, false, true, false);
                }
            }
            if (info.hFile) CloseHandle(info.hFile);
            break;
        }
        case CREATE_THREAD_DEBUG_EVENT:
            threads_[ev.dwThreadId] = ev.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            threads_.erase(ev.dwThreadId);
            break;
        case LOAD_DLL_DEBUG_EVENT: {
            const auto& info = ev.u.LoadDll;
            uint64_t base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
            std::wstring name = L"dll";
            if (info.hFile) {
                wchar_t path[MAX_PATH * 2] = L"";
                if (GetFinalPathNameByHandleW(info.hFile, path, static_cast<DWORD>(std::size(path)), FILE_NAME_NORMALIZED)) {
                    name = fs::path(path).stem().wstring();
                }
                CloseHandle(info.hFile);
            }
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            modules_[base] = {base, ImageSize(base), name};
            break;
        }
        case UNLOAD_DLL_DEBUG_EVENT: {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            modules_.erase(reinterpret_cast<uint64_t>(ev.u.UnloadDll.lpBaseOfDll));
            break;
        }
        case OUTPUT_DEBUG_STRING_EVENT: {
            const auto& info = ev.u.DebugString;
            size_t chars = std::min<size_t>(info.nDebugStringLength, 4096);
            std::string text;
            if (info.fUnicode) {
                std::wstring w(chars, L'\0');
                SIZE_T n = 0;
                ReadProcessMemory(process_, info.lpDebugStringData, w.data(), chars * 2, &n);
                text = Narrow(w.c_str());
            } else {
                std::string a(chars, '\0');
                SIZE_T n = 0;
                ReadProcessMemory(process_, info.lpDebugStringData, a.data(), chars, &n);
                text = Narrow(Widen(a.c_str(), CP_ACP));
            }
            Log(notify_, "[debug] " + text + (text.empty() || text.back() != '\n' ? "\n" : ""), Out::Info);
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
            status = OnException(ev);
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            exitCode = ev.u.ExitProcess.dwExitCode;
            done = true;
            break;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        SymCleanup(process_);
        process_ = nullptr;
        breakpoints_.clear();
        fileBreakpoints_.clear();
    }
    stopped_ = false;
    running_ = false;
    PostMessageW(notify_, WM_DEBUG_EXITED, exitCode, 0);
}

DWORD Debugger::OnException(const DEBUG_EVENT& ev) {
    const EXCEPTION_RECORD& rec = ev.u.Exception.ExceptionRecord;
    auto threadIt = threads_.find(ev.dwThreadId);
    HANDLE thread = threadIt != threads_.end() ? threadIt->second : mainThread_;
    uint64_t address = reinterpret_cast<uint64_t>(rec.ExceptionAddress);
    alignas(16) CONTEXT ctx;

    switch (rec.ExceptionCode) {
    case EXCEPTION_BREAKPOINT: {
        if (!loaderBreakpointSeen_) {  // the loader's initial breakpoint in ntdll
            loaderBreakpointSeen_ = true;
            return DBG_CONTINUE;
        }
        Breakpoint bp;
        bool found = false, wasRemoved = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto it = breakpoints_.find(address);
            if (it != breakpoints_.end()) {
                found = true;
                bp = it->second;
                if (bp.entry) {
                    it->second.entry = false;
                    if (!bp.user && !bp.temp) Remove(address);
                }
            } else {
                wasRemoved = removed_.erase(address) > 0;
            }
        }
        if (!found && pauseRequested_ && !wasRemoved) {  // DebugBreakProcess runs int3 in a helper thread
            pauseRequested_ = false;
            stepMode_ = StepMode::None;
            GetContext(mainThread_, ctx);
            StopAt(mainThread_, ctx, DebugStop::Reason::Pause, L"");
            return DBG_CONTINUE;
        }
        GetContext(thread, ctx);
        if (!found) {
            if (wasRemoved) {  // hit a breakpoint the user removed a moment ago
                ctx.Rip = address;
                SetContext(thread, ctx);
                return DBG_CONTINUE;
            }
            stepMode_ = StepMode::None;
            StopAt(thread, ctx, DebugStop::Reason::Int3, L"");
            return DBG_CONTINUE;
        }
        ctx.Rip = address;  // re-execute the original instruction
        if (bp.user || bp.entry) {
            stepMode_ = StepMode::None;
            StopAt(thread, ctx, bp.entry && !bp.user ? DebugStop::Reason::Entry : DebugStop::Reason::Breakpoint, L"");
        } else if (pauseRequested_) {
            pauseRequested_ = false;
            stepMode_ = StepMode::None;
            StopAt(thread, ctx, DebugStop::Reason::Pause, L"");
        } else {
            RemoveTemps();
            if (stepMode_ == StepMode::Out) ContinueStepping(thread, ctx);
            else if (stepMode_ != StepMode::None) CheckStop(thread, ctx);
            else RunFree(thread, ctx);
        }
        return DBG_CONTINUE;
    }
    case EXCEPTION_SINGLE_STEP: {
        GetContext(thread, ctx);
        if (reinsertAt_) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (breakpoints_.count(reinsertAt_)) {
                uint8_t cc = 0xCC;
                WriteProcessMemory(process_, reinterpret_cast<void*>(reinsertAt_), &cc, 1, nullptr);
                FlushInstructionCache(process_, reinterpret_cast<void*>(reinsertAt_), 1);
            }
            reinsertAt_ = 0;
        }
        if (stepMode_ == StepMode::None) Resume(thread, ctx, false);
        else OnStep(thread, ctx);
        return DBG_CONTINUE;
    }
    case kStatusWx86Breakpoint:
        return DBG_CONTINUE;
    default: {
        if ((rec.ExceptionCode & 0xF0000000) != 0xC0000000) return DBG_EXCEPTION_NOT_HANDLED;  // informational
        GetContext(thread, ctx);
        stepMode_ = StepMode::None;
        std::wstring msg = (ev.u.Exception.dwFirstChance ? L"Исключение " : L"Необработанное исключение ") +
                           Toolchain::DescribeExitCode(rec.ExceptionCode);
        if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
            wchar_t buf[96];
            const wchar_t* kind = rec.ExceptionInformation[0] == 0 ? L"чтение" : rec.ExceptionInformation[0] == 1 ? L"запись" : L"выполнение";
            swprintf_s(buf, L"; %s по адресу 0x%llX", kind, static_cast<unsigned long long>(rec.ExceptionInformation[1]));
            msg += buf;
        }
        StopAt(thread, ctx, DebugStop::Reason::Exception, msg);
        return DBG_EXCEPTION_NOT_HANDLED;
    }
    }
}

// Called after every single-stepped instruction while a step command is active.
void Debugger::OnStep(HANDLE thread, CONTEXT& ctx) {
    // Detect "call": rsp dropped by 8 and the pushed value points just past the previous instruction.
    uint64_t pushed = ctx.Rsp == prevRsp_ - 8 ? ReadQword(ctx.Rsp) : 0;
    bool isCall = pushed > prevRip_ && pushed <= prevRip_ + 15 && ctx.Rip != pushed;
    bool isRet = ctx.Rsp > prevRsp_ && ReadQword(prevRsp_) == ctx.Rip;

    switch (stepMode_) {
    case StepMode::Into:
        if (isCall && !LocationOf(ctx.Rip).valid()) {  // into a function without source: run until it returns
            Install(pushed, false, false, true);
            Resume(thread, ctx, false);
            return;
        }
        CheckStop(thread, ctx);
        return;
    case StepMode::Over:
        if (isCall) {
            Install(pushed, false, false, true);
            Resume(thread, ctx, false);
            return;
        }
        CheckStop(thread, ctx);
        return;
    case StepMode::Out:
        if (isCall) {
            Install(pushed, false, false, true);
            Resume(thread, ctx, false);
            return;
        }
        if (isRet) {
            stepMode_ = StepMode::Into;
            stepFrom_ = {};
            CheckStop(thread, ctx);
            return;
        }
        ContinueStepping(thread, ctx);
        return;
    case StepMode::None:
        Resume(thread, ctx, false);
        return;
    }
}

void Debugger::CheckStop(HANDLE thread, CONTEXT& ctx) {
    SourceLocation loc = LocationOf(ctx.Rip);
    if (!loc.valid()) {
        RunFree(thread, ctx);  // e.g. returned from main into the C runtime
    } else if (!(loc == stepFrom_)) {
        stepMode_ = StepMode::None;
        StopAt(thread, ctx, DebugStop::Reason::Step, L"");
    } else {
        ContinueStepping(thread, ctx);  // still on the same source line
    }
}

void Debugger::StopAt(HANDLE thread, CONTEXT& ctx, DebugStop::Reason reason, const std::wstring& message) {
    pauseRequested_ = false;
    RemoveTemps();
    context_ = ctx;
    auto* stop = new DebugStop{reason, message, LocationOf(ctx.Rip), ctx};
    stopped_ = true;
    PostMessageW(notify_, WM_DEBUG_STOPPED, 0, reinterpret_cast<LPARAM>(stop));

    Command cmd;
    {
        std::unique_lock<std::mutex> lock(commandMutex_);
        cv_.wait(lock, [this] { return command_ != Command::None; });
        cmd = command_;
        if (cmd != Command::Stop) command_ = Command::None;
    }
    stopped_ = false;
    switch (cmd) {
    case Command::StepInto:
    case Command::StepOver:
    case Command::StepOut:
        stepMode_ = cmd == Command::StepInto ? StepMode::Into : cmd == Command::StepOver ? StepMode::Over : StepMode::Out;
        stepFrom_ = LocationOf(ctx.Rip);
        ContinueStepping(thread, ctx);
        break;
    case Command::Stop:
        TerminateProcess(process_, 1);
        break;
    default:
        RunFree(thread, ctx);
        break;
    }
}

void Debugger::Resume(HANDLE thread, CONTEXT& ctx, bool singleStep) {
    {
        // A breakpoint at the resume address: execute the original byte, then put int3 back after one step.
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = breakpoints_.find(ctx.Rip);
        if (it != breakpoints_.end()) {
            WriteProcessMemory(process_, reinterpret_cast<void*>(ctx.Rip), &it->second.original, 1, nullptr);
            FlushInstructionCache(process_, reinterpret_cast<void*>(ctx.Rip), 1);
            reinsertAt_ = ctx.Rip;
            singleStep = true;
        }
    }
    if (singleStep) ctx.EFlags |= kTrapFlag;
    else ctx.EFlags &= ~kTrapFlag;
    SetContext(thread, ctx);
}

void Debugger::ContinueStepping(HANDLE thread, CONTEXT& ctx) {
    prevRip_ = ctx.Rip;
    prevRsp_ = ctx.Rsp;
    Resume(thread, ctx, true);
}

void Debugger::RunFree(HANDLE thread, CONTEXT& ctx) {
    stepMode_ = StepMode::None;
    Resume(thread, ctx, false);
}

// ---- breakpoints ------------------------------------------------------------------------------------------------

bool Debugger::Install(uint64_t address, bool user, bool entry, bool temp) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = breakpoints_.find(address);
    if (it != breakpoints_.end()) {
        it->second.user |= user;
        it->second.entry |= entry;
        it->second.temp |= temp;
        return true;
    }
    Breakpoint bp{address, 0, user, entry, temp};
    SIZE_T n = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<void*>(address), &bp.original, 1, &n) || n != 1) return false;
    uint8_t cc = 0xCC;
    if (!WriteProcessMemory(process_, reinterpret_cast<void*>(address), &cc, 1, &n)) return false;
    FlushInstructionCache(process_, reinterpret_cast<void*>(address), 1);
    breakpoints_[address] = bp;
    removed_.erase(address);
    return true;
}

void Debugger::Remove(uint64_t address) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = breakpoints_.find(address);
    if (it == breakpoints_.end()) return;
    if (reinsertAt_ != address) {
        WriteProcessMemory(process_, reinterpret_cast<void*>(address), &it->second.original, 1, nullptr);
        FlushInstructionCache(process_, reinterpret_cast<void*>(address), 1);
    }
    breakpoints_.erase(it);
    if (!stopped_) removed_.insert(address);
}

void Debugger::RemoveTemps() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<uint64_t> drop;
    for (auto& [addr, bp] : breakpoints_) {
        if (!bp.temp) continue;
        bp.temp = false;
        if (!bp.user && !bp.entry) drop.push_back(addr);
    }
    for (uint64_t a : drop) Remove(a);
}

void Debugger::InstallFile(const std::wstring& key, const std::set<int>& lines) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto& addrs = fileBreakpoints_[key];
    for (int line : lines) {
        LONG displacement = 0;
        IMAGEHLP_LINEW64 info{sizeof(info)};
        if (!SymGetLineFromNameW64(process_, nullptr, key.c_str(), line, &displacement, &info)) continue;
        if (!SamePath(info.FileName, key) || static_cast<int>(info.LineNumber) < line ||
            static_cast<int>(info.LineNumber) > line + 20) {
            continue;  // no code on or shortly after this line
        }
        if (Install(info.Address, true, false, false)) addrs.push_back(info.Address);
    }
}

void Debugger::RemoveUserFile(const std::wstring& key) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = fileBreakpoints_.find(key);
    if (it == fileBreakpoints_.end()) return;
    for (uint64_t addr : it->second) {
        auto bp = breakpoints_.find(addr);
        if (bp == breakpoints_.end()) continue;
        bp->second.user = false;
        if (!bp->second.entry && !bp->second.temp) Remove(addr);
    }
    fileBreakpoints_.erase(it);
}

// ---- inspection -------------------------------------------------------------------------------------------------

uint64_t Debugger::ImageSize(uint64_t base) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (ReadMemory(base, &dos, sizeof(dos)) != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0x1000;
    if (ReadMemory(base + dos.e_lfanew, &nt, sizeof(nt)) != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE) return 0x1000;
    return nt.OptionalHeader.SizeOfImage;
}

SourceLocation Debugger::LocationOf(uint64_t address) {
    if (!InMainImage(address)) return {};  // source exists only for our own executable
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    DWORD displacement = 0;
    IMAGEHLP_LINEW64 line{sizeof(line)};
    if (process_ && SymGetLineFromAddrW64(process_, address, &displacement, &line) && FileExists(line.FileName)) {
        return {NormalizePath(line.FileName), static_cast<int>(line.LineNumber)};
    }
    return {};
}

uint64_t Debugger::ReadQword(uint64_t address) {
    uint64_t v = 0;
    SIZE_T n = 0;
    ReadProcessMemory(process_, reinterpret_cast<void*>(address), &v, 8, &n);
    return n == 8 ? v : 0;
}

size_t Debugger::ReadMemory(uint64_t address, void* buffer, size_t size) {
    HANDLE p = process_;
    if (!p) return 0;
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {  // page by page, so a readable prefix is still returned
        uint64_t a = address + done;
        size_t chunk = std::min<size_t>(size - done, 0x1000 - (a & 0xFFF));
        SIZE_T n = 0;
        if (!ReadProcessMemory(p, reinterpret_cast<void*>(a), out + done, chunk, &n) || n == 0) break;
        done += n;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& [addr, bp] : breakpoints_) {  // show original bytes instead of int3
        if (addr >= address && addr < address + done && addr != reinsertAt_) out[addr - address] = bp.original;
    }
    return done;
}

std::optional<uint64_t> Debugger::Evaluate(const std::string& expression, std::wstring& error) {
    return ExpressionParser(*this, expression, error).Parse();
}

std::optional<uint64_t> Debugger::LookupSymbol(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!process_) return std::nullopt;
    SymbolBuffer sym;
    if (SymFromNameW(process_, Widen(name).c_str(), &sym.info)) return sym.info.Address;
    return std::nullopt;
}

std::wstring Debugger::SymbolAt(uint64_t address) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!process_ || address < 0x10000) return {};
    wchar_t buf[32];
    if (!InMainImage(address)) {
        auto it = modules_.upper_bound(address);
        if (it == modules_.begin()) return {};
        --it;
        if (address >= it->second.base + it->second.size) return {};
        swprintf_s(buf, L"+0x%llX", static_cast<unsigned long long>(address - it->second.base));
        return it->second.name + buf;
    }
    SymbolBuffer sym;
    DWORD64 displacement = 0;
    if (!SymFromAddrW(process_, address, &displacement, &sym.info) || displacement > 0x4000) return {};
    std::wstring name = sym.info.Name;
    if (name.rfind(L"__ImageBase", 0) == 0) return {};
    if (displacement) {
        swprintf_s(buf, L"+0x%llX", static_cast<unsigned long long>(displacement));
        name += buf;
    }
    return name;
}
