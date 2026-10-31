#include "debugger.h"
#include "gdbmi.h"

#include <dbghelp.h>

namespace {

constexpr DWORD kTrapFlag = 0x100;
constexpr DWORD kStatusWx86SingleStep = 0x4000001E;
constexpr DWORD kStatusWx86Breakpoint = 0x4000001F;
constexpr uint64_t kProfileLimit = 1000000;  // traced instructions (~0.3 ms each); then the program runs untraced

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

void PostLog(HWND hwnd, const std::string& text, Out style) {
    PostMessageW(hwnd, WM_DEBUG_LOG, static_cast<WPARAM>(style), reinterpret_cast<LPARAM>(new std::string(text)));
}

// Debug-event trace for diagnosing the debugger itself (set ASMEDITOR_TRACE=1).
void Trace(HWND hwnd, const char* what, uint64_t a = 0, uint64_t b = 0) {
    static const bool enabled = [] {
        size_t n = 0;
        return getenv_s(&n, nullptr, 0, "ASMEDITOR_TRACE") == 0 && n > 0;
    }();
    char buf[160];
    sprintf_s(buf, "[trace %lu] %s %llX %llX\n", GetTickCount() % 100000, what, static_cast<unsigned long long>(a),
              static_cast<unsigned long long>(b));
    if (Log::Enabled()) Log::Write(buf);
    if (enabled) PostLog(hwnd, buf, Out::Info);
}

// Small expression language for the memory view, watches, conditions and hover tips:
//   numbers (42, 0x2A, 2Ah, 0b101), registers, symbols, + - * / %, & | ^ << >>, comparisons (== != < > <= >=,
//   signed), && ||, !, (expr), [expr] = pointer-sized value at the address, byte/word/dword/qword [expr].
class ExpressionParser {
public:
    ExpressionParser(Debugger& d, const std::string& s, std::wstring& err) : d_(d), s_(s), err_(err) {}

    std::optional<uint64_t> Parse() {
        auto v = Or();
        Skip();
        if (v && i_ < s_.size()) {
            err_ = Tr(L"лишние символы: ") + Widen(s_.substr(i_));
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
    bool Eat(const char* op) {
        Skip();
        size_t n = strlen(op);
        if (s_.compare(i_, n, op) != 0) return false;
        // "<" must not eat the start of "<<" or "<=", "&" not of "&&", etc.
        if (n == 1 && i_ + 1 < s_.size()) {
            char next = s_[i_ + 1];
            if ((op[0] == '<' || op[0] == '>') && (next == op[0] || next == '=')) return false;
            if ((op[0] == '&' || op[0] == '|') && next == op[0]) return false;
            if ((op[0] == '=' || op[0] == '!') && next == '=') return false;
        }
        i_ += n;
        return true;
    }
    template <typename Next, typename Apply>
    std::optional<uint64_t> Binary(Next next, std::initializer_list<const char*> ops, Apply apply) {
        auto v = (this->*next)();
        while (v) {
            const char* matched = nullptr;
            for (const char* op : ops) {
                if (Eat(op)) {
                    matched = op;
                    break;
                }
            }
            if (!matched) break;
            auto r = (this->*next)();
            if (!r) return std::nullopt;
            auto result = apply(matched, *v, *r);
            if (!result) return std::nullopt;
            v = result;
        }
        return v;
    }
    std::optional<uint64_t> Or() {
        return Binary(&ExpressionParser::And, {"||"}, [](const char*, uint64_t a, uint64_t b) {
            return std::optional<uint64_t>(a || b);
        });
    }
    std::optional<uint64_t> And() {
        return Binary(&ExpressionParser::Compare, {"&&"}, [](const char*, uint64_t a, uint64_t b) {
            return std::optional<uint64_t>(a && b);
        });
    }
    std::optional<uint64_t> Compare() {
        return Binary(&ExpressionParser::BitOr, {"==", "!=", "<=", ">=", "<", ">"}, [](const char* op, uint64_t a, uint64_t b) {
            auto x = static_cast<int64_t>(a), y = static_cast<int64_t>(b);
            std::string o = op;
            bool r = o == "==" ? a == b : o == "!=" ? a != b : o == "<=" ? x <= y : o == ">=" ? x >= y : o == "<" ? x < y : x > y;
            return std::optional<uint64_t>(r);
        });
    }
    std::optional<uint64_t> BitOr() {
        return Binary(&ExpressionParser::BitXor, {"|"}, [](const char*, uint64_t a, uint64_t b) {
            return std::optional<uint64_t>(a | b);
        });
    }
    std::optional<uint64_t> BitXor() {
        return Binary(&ExpressionParser::BitAnd, {"^"}, [](const char*, uint64_t a, uint64_t b) {
            return std::optional<uint64_t>(a ^ b);
        });
    }
    std::optional<uint64_t> BitAnd() {
        return Binary(&ExpressionParser::Shift, {"&"}, [](const char*, uint64_t a, uint64_t b) {
            return std::optional<uint64_t>(a & b);
        });
    }
    std::optional<uint64_t> Shift() {
        return Binary(&ExpressionParser::Expr, {"<<", ">>"}, [](const char* op, uint64_t a, uint64_t b) {
            return std::optional<uint64_t>(b >= 64 ? 0 : op[0] == '<' ? a << b : a >> b);
        });
    }
    std::optional<uint64_t> Expr() {
        return Binary(&ExpressionParser::Term, {"+", "-"}, [](const char* op, uint64_t a, uint64_t b) {
            return std::optional<uint64_t>(op[0] == '+' ? a + b : a - b);
        });
    }
    std::optional<uint64_t> Term() {
        return Binary(&ExpressionParser::Factor, {"*", "/", "%"}, [this](const char* op, uint64_t a, uint64_t b) {
            if (op[0] == '*') return std::optional<uint64_t>(a * b);
            if (b == 0) {
                err_ = Tr(L"деление на ноль");
                return std::optional<uint64_t>();
            }
            auto x = static_cast<int64_t>(a), y = static_cast<int64_t>(b);
            return std::optional<uint64_t>(op[0] == '/' ? static_cast<uint64_t>(x / y) : static_cast<uint64_t>(x % y));
        });
    }
    std::optional<uint64_t> Deref(int size) {
        auto v = Or();
        if (!v) return std::nullopt;
        if (!Eat("]")) {
            err_ = Tr(L"ожидалась «]»");
            return std::nullopt;
        }
        uint64_t q = 0;
        if (d_.ReadMemory(*v, &q, size) != static_cast<size_t>(size)) {
            err_ = Tr(L"память по адресу недоступна");
            return std::nullopt;
        }
        return q;
    }
    std::optional<uint64_t> Factor() {
        Skip();
        if (i_ >= s_.size()) {
            err_ = Tr(L"ожидалось выражение");
            return std::nullopt;
        }
        if (Eat("-")) {
            auto v = Factor();
            if (v) *v = 0 - *v;
            return v;
        }
        if (Eat("!")) {
            auto v = Factor();
            if (v) *v = !*v;
            return v;
        }
        if (Eat("~")) {
            auto v = Factor();
            if (v) *v = ~*v;
            return v;
        }
        if (Eat("(")) {
            auto v = Or();
            if (v && !Eat(")")) {
                err_ = Tr(L"ожидалась «)»");
                return std::nullopt;
            }
            return v;
        }
        if (Eat("[")) return Deref(d_.PointerSize());
        size_t start = i_;
        auto isWord = [](char c) {
            return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' ||
                   static_cast<unsigned char>(c) >= 0x80;
        };
        while (i_ < s_.size() && isWord(s_[i_])) i_++;
        std::string word = s_.substr(start, i_ - start);
        if (word.empty()) {
            err_ = Tr(L"неожиданный символ: ") + Widen(std::string(1, s_[i_]));
            return std::nullopt;
        }
        if (isdigit(static_cast<unsigned char>(word[0]))) return Number(word);
        std::string lower = LowerAscii(word);
        static const std::pair<const char*, int> sizes[] = {{"byte", 1}, {"word", 2}, {"dword", 4}, {"qword", 8}};
        for (auto [name, size] : sizes) {
            if (lower == name) {
                Eat("ptr");
                if (!Eat("[")) {
                    err_ = Widen(word) + Tr(L": ожидалась «[»");
                    return std::nullopt;
                }
                return Deref(size);
            }
        }
        if (auto r = Debugger::Register(d_.Context(), lower)) return r;
        if (auto a = d_.LookupSymbol(word)) return a;
        err_ = Tr(L"неизвестное имя: ") + Widen(word);
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
            err_ = Tr(L"неверное число: ") + Widen(w);
            return std::nullopt;
        }
        return v;
    }
};

// Length of an indirect "call r/m" (FF /2) whose encoding ends right before `end`, or 0.
bool EndsWithIndirectCall(const uint8_t* bytes, size_t avail, bool is32) {
    for (size_t len = 2; len <= 7 && len <= avail; len++) {
        const uint8_t* p = bytes + avail - len;
        size_t i = 0;
        if (!is32 && (p[0] & 0xF0) == 0x40) i++;  // REX
        if (p[i] != 0xFF || i + 1 >= len) continue;
        uint8_t modrm = p[i + 1];
        if (((modrm >> 3) & 7) != 2) continue;
        int mod = modrm >> 6, rm = modrm & 7;
        size_t n = i + 2;
        if (mod != 3 && rm == 4) {  // SIB
            if (n >= len + 1) continue;
            uint8_t sib = p[n];
            n++;
            if (mod == 0 && (sib & 7) == 5) n += 4;
        }
        if (mod == 1) n += 1;
        else if (mod == 2) n += 4;
        else if (mod == 0 && rm == 5) n += 4;
        if (n == len) return true;
    }
    return false;
}
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
    if (n == "rflags" || n == "eflags" || n == "flags") return c.EFlags;
    return std::nullopt;
}

// Writes a register (or a part of one, like the hardware does: eax clears the upper half of rax, ax and al do not).
bool Debugger::SetRegisterIn(CONTEXT& c, const std::string& n, uint64_t v) {
    DWORD64* r[] = {&c.Rax, &c.Rbx, &c.Rcx, &c.Rdx, &c.Rsi, &c.Rdi, &c.Rbp, &c.Rsp,
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
        if (n == n64[i]) *r[i] = v;
        else if (n == n32[i]) *r[i] = v & 0xFFFFFFFF;
        else if (n == n16[i]) *r[i] = (*r[i] & ~0xFFFFull) | (v & 0xFFFF);
        else if (n == n8[i]) *r[i] = (*r[i] & ~0xFFull) | (v & 0xFF);
        else continue;
        return true;
    }
    DWORD64* high[] = {&c.Rax, &c.Rbx, &c.Rcx, &c.Rdx};
    static const char* n8h[] = {"ah", "bh", "ch", "dh"};
    for (int i = 0; i < 4; i++) {
        if (n == n8h[i]) {
            *high[i] = (*high[i] & ~0xFF00ull) | ((v & 0xFF) << 8);
            return true;
        }
    }
    if (n == "rip" || n == "eip") {
        c.Rip = n == "eip" ? (v & 0xFFFFFFFF) : v;
        return true;
    }
    if (n == "rflags" || n == "eflags" || n == "flags") {
        c.EFlags = static_cast<DWORD>((c.EFlags & ~0xCD5u) | (v & 0xCD5u));  // CF PF AF ZF SF DF OF only
        return true;
    }
    return false;
}

Debugger::Debugger(HWND notify) : notify_(notify) {}

Debugger::~Debugger() {
    sampling_ = false;
    if (sampler_.joinable()) sampler_.join();
    Stop();
    gdb_.reset();
    if (thread_.joinable()) thread_.join();
}

// ---- thread context ---------------------------------------------------------------------------------------------

// Context changes are only made on the thread that reported the debug event: on another thread they can block
// for seconds (see Pause()). A 32-bit (WOW64) thread's context is mapped onto the x64 CONTEXT fields
// (eax -> Rax, eip -> Rip...), so the rest of the debugger and the UI work with one layout.
bool Debugger::GetContext(HANDLE thread, CONTEXT& ctx) {
    ZeroMemory(&ctx, sizeof(ctx));
    if (!wow64_) {
        ctx.ContextFlags = CONTEXT_ALL;
        return GetThreadContext(thread, &ctx) != FALSE;
    }
    WOW64_CONTEXT w{};
    w.ContextFlags = WOW64_CONTEXT_ALL;
    if (!Wow64GetThreadContext(thread, &w)) return false;
    ctx.ContextFlags = CONTEXT_ALL;
    ctx.Rax = w.Eax;
    ctx.Rbx = w.Ebx;
    ctx.Rcx = w.Ecx;
    ctx.Rdx = w.Edx;
    ctx.Rsi = w.Esi;
    ctx.Rdi = w.Edi;
    ctx.Rbp = w.Ebp;
    ctx.Rsp = w.Esp;
    ctx.Rip = w.Eip;
    ctx.EFlags = w.EFlags;
    ctx.Dr0 = w.Dr0;
    ctx.Dr1 = w.Dr1;
    ctx.Dr2 = w.Dr2;
    ctx.Dr3 = w.Dr3;
    ctx.Dr6 = w.Dr6;
    ctx.Dr7 = w.Dr7;
    static_assert(sizeof(w.ExtendedRegisters) == sizeof(ctx.FltSave), "both are FXSAVE images");
    memcpy(&ctx.FltSave, w.ExtendedRegisters, sizeof(ctx.FltSave));
    return true;
}

bool Debugger::SetContext(HANDLE thread, const CONTEXT& ctx, bool full) {
    if (!wow64_) {
        alignas(16) CONTEXT out = ctx;
        out.ContextFlags = (full ? CONTEXT_ALL : CONTEXT_CONTROL) | CONTEXT_DEBUG_REGISTERS;
        return SetThreadContext(thread, &out) != FALSE;
    }
    WOW64_CONTEXT w{};
    w.ContextFlags = WOW64_CONTEXT_ALL;
    if (!Wow64GetThreadContext(thread, &w)) return false;
    w.Eip = static_cast<DWORD>(ctx.Rip);
    w.Esp = static_cast<DWORD>(ctx.Rsp);
    w.Ebp = static_cast<DWORD>(ctx.Rbp);
    w.EFlags = ctx.EFlags;
    w.Dr0 = static_cast<DWORD>(ctx.Dr0);
    w.Dr1 = static_cast<DWORD>(ctx.Dr1);
    w.Dr2 = static_cast<DWORD>(ctx.Dr2);
    w.Dr3 = static_cast<DWORD>(ctx.Dr3);
    w.Dr6 = static_cast<DWORD>(ctx.Dr6);
    w.Dr7 = static_cast<DWORD>(ctx.Dr7);
    w.ContextFlags = WOW64_CONTEXT_CONTROL | WOW64_CONTEXT_DEBUG_REGISTERS;
    if (full) {
        w.Eax = static_cast<DWORD>(ctx.Rax);
        w.Ebx = static_cast<DWORD>(ctx.Rbx);
        w.Ecx = static_cast<DWORD>(ctx.Rcx);
        w.Edx = static_cast<DWORD>(ctx.Rdx);
        w.Esi = static_cast<DWORD>(ctx.Rsi);
        w.Edi = static_cast<DWORD>(ctx.Rdi);
        memcpy(w.ExtendedRegisters, &ctx.FltSave, sizeof(w.ExtendedRegisters));
        w.ContextFlags = WOW64_CONTEXT_ALL;
    }
    return Wow64SetThreadContext(thread, &w) != FALSE;
}

// ---- commands (UI thread) ---------------------------------------------------------------------------------------

bool Debugger::Start(const LaunchOptions& launch, HPCON console, const Breakpoints& breakpoints, bool stopAtEntry,
                     std::optional<SourceLocation> runTo, std::wstring& error) {
    if (launch.wsl) {  // a Linux program: gdb in WSL does the work
        gdb_ = std::make_unique<GdbSession>(*this, notify_);
        running_ = true;
        if (!gdb_->Start(launch, launch.wslTty, breakpoints, stopAtEntry, runTo, error)) {
            gdb_.reset();
            running_ = false;
            return false;
        }
        return true;
    }
    std::promise<std::wstring> started;
    auto result = started.get_future();
    running_ = true;
    thread_ = std::thread(&Debugger::Loop, this, launch, console, breakpoints, stopAtEntry, runTo, &started);
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

void Debugger::Continue() {
    if (gdb_) gdb_->Exec("-exec-continue");
    else Send(Command::Continue);
}

void Debugger::StepInto() {
    if (gdb_) gdb_->Exec("-exec-step");
    else Send(Command::StepInto);
}

void Debugger::StepOver() {
    if (gdb_) gdb_->Exec("-exec-next");
    else Send(Command::StepOver);
}

void Debugger::StepOut() {
    if (gdb_) gdb_->Exec("-exec-finish");
    else Send(Command::StepOut);
}

void Debugger::Pause() {
    if (gdb_) return gdb_->Interrupt();
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
    if (gdb_) return gdb_->Stop();
    if (attached_) {  // detach (in the debug loop), the program keeps running
        detachRequested_ = true;
        Send(Command::Stop);
        return;
    }
    Send(Command::Stop);
    HANDLE p = process_;
    if (running_ && p) TerminateProcess(p, 1);
}

void Debugger::SetBreakpoints(const fs::path& file, const std::map<int, std::string>& lines) {
    if (gdb_) return gdb_->SetBreakpoints(file, lines);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!process_) return;
    std::wstring key = Key(file);
    RemoveUserFile(key);
    InstallFile(key, lines);
}

// ---- step back --------------------------------------------------------------------------------------------------

size_t Debugger::Snapshot::Bytes() const {
    size_t n = sizeof(*this);
    for (const auto& [a, bytes] : memory) n += bytes.size();
    return n;
}

// The writable sections of the program's image (.data, .bss): where its global variables live.
void Debugger::FindWritableSections() {
    writable_.clear();
    IMAGE_DOS_HEADER dos{};
    if (ReadMemory(imageBase_, &dos, sizeof(dos)) != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_FILE_HEADER file{};
    uint64_t at = imageBase_ + dos.e_lfanew + 4;
    if (ReadMemory(at, &file, sizeof(file)) != sizeof(file)) return;
    at += sizeof(file) + file.SizeOfOptionalHeader;
    for (int i = 0; i < file.NumberOfSections && i < 96; i++) {
        IMAGE_SECTION_HEADER sec{};
        if (ReadMemory(at + i * sizeof(sec), &sec, sizeof(sec)) != sizeof(sec)) break;
        if (!(sec.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uint64_t size = std::max<uint64_t>(sec.Misc.VirtualSize, sec.SizeOfRawData);
        if (size && size <= (16u << 20)) writable_.push_back({imageBase_ + sec.VirtualAddress, size});
    }
}

// Base addresses of the program's heaps, from PEB.ProcessHeaps (64-bit programs only).
std::vector<uint64_t> Debugger::HeapBases() {
    std::vector<uint64_t> out;
    if (wow64_) return out;
    using Query = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    struct BasicInformation {
        void* reserved1;
        void* peb;
        void* reserved2[2];
        ULONG_PTR pid;
        void* reserved3;
    } info{};
    if (!query || query(process_, 0, &info, sizeof(info), nullptr) != 0 || !info.peb) return out;
    uint64_t peb = reinterpret_cast<uint64_t>(info.peb);
    uint32_t count = 0;
    uint64_t heaps = 0;
    if (ReadMemory(peb + 0xE8, &count, 4) != 4 || ReadMemory(peb + 0xF0, &heaps, 8) != 8) return out;
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 16); i++) {
        uint64_t h = 0;
        if (ReadMemory(heaps + i * 8ull, &h, 8) == 8 && h) out.push_back(h);
    }
    return out;
}

void Debugger::RecordSnapshot(HANDLE thread, const CONTEXT& ctx) {
    constexpr size_t kMaxSnapshots = 2000, kMaxBytes = 256u << 20, kStackBytes = 64u << 10;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Snapshot snap;
    snap.thread = thread;
    snap.context = ctx;
    for (auto [address, size] : writable_) {
        std::vector<uint8_t> bytes(size);
        bytes.resize(ReadMemory(address, bytes.data(), size));
        snap.memory.push_back({address, std::move(bytes)});
    }
    // The heaps (malloc, HeapAlloc): the committed parts of each heap's first segment, up to 8 MB in all.
    size_t heapBytes = 0;
    for (uint64_t base : HeapBases()) {
        for (uint64_t a = base; heapBytes < (8u << 20);) {
            MEMORY_BASIC_INFORMATION r{};
            if (!VirtualQueryEx(process_, reinterpret_cast<void*>(a), &r, sizeof(r)) ||
                reinterpret_cast<uint64_t>(r.AllocationBase) != base) {
                break;
            }
            uint64_t next = reinterpret_cast<uint64_t>(r.BaseAddress) + r.RegionSize;
            if (r.State == MEM_COMMIT && (r.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(r.Protect & PAGE_GUARD)) {
                size_t size = std::min<size_t>(r.RegionSize, (8u << 20) - heapBytes);
                std::vector<uint8_t> bytes(size);
                bytes.resize(ReadMemory(reinterpret_cast<uint64_t>(r.BaseAddress), bytes.data(), size));
                heapBytes += bytes.size();
                snap.memory.push_back({reinterpret_cast<uint64_t>(r.BaseAddress), std::move(bytes)});
            }
            a = next;
        }
    }
    // The used part of the stack: from a little below rsp up to the end of its committed region.
    MEMORY_BASIC_INFORMATION mbi{};
    uint64_t from = (ctx.Rsp - 256) & ~15ull;
    if (VirtualQueryEx(process_, reinterpret_cast<void*>(ctx.Rsp), &mbi, sizeof(mbi))) {
        uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        from = std::max<uint64_t>(from, reinterpret_cast<uint64_t>(mbi.BaseAddress));
        std::vector<uint8_t> bytes(std::min<uint64_t>(end - from, kStackBytes));
        bytes.resize(ReadMemory(from, bytes.data(), bytes.size()));
        snap.memory.push_back({from, std::move(bytes)});
    }
    history_.push_back(std::move(snap));
    size_t total = 0;
    for (const Snapshot& s : history_) total += s.Bytes();
    while (history_.size() > 1 && (history_.size() > kMaxSnapshots || total > kMaxBytes)) {
        total -= history_.front().Bytes();
        history_.pop_front();
    }
}

size_t Debugger::HistorySize() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return history_.size();
}

bool Debugger::StepBack(std::wstring& error) {
    if (!stopped_) return false;
    if (gdb_) {
        error = Tr(L"в WSL (gdb) шаг назад не поддерживается");
        return false;
    }
    if (foreignStop_) {
        error = Tr(L"программа остановлена внутри системного вызова — сначала сделайте шаг");
        return false;
    }
    DebugStop* stop = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (history_.size() < 2) {
            error = Tr(L"нет более ранней остановки");
            return false;
        }
        const Snapshot& prev = history_[history_.size() - 2];
        if (prev.thread != stopThread_) {
            error = Tr(L"предыдущая остановка была в другом потоке");
            return false;
        }
        history_.pop_back();
        for (const auto& [address, bytes] : prev.memory) {
            // Write only what differs: most of the image's data is unchanged.
            std::vector<uint8_t> now(bytes.size());
            size_t got = std::min(ReadMemory(address, now.data(), now.size()), bytes.size());
            for (size_t i = 0; i < got;) {
                if (now[i] == bytes[i]) {
                    i++;
                    continue;
                }
                size_t j = i;
                while (j < got && now[j] != bytes[j]) j++;
                WriteMemory(address + i, bytes.data() + i, j - i);
                i = j;
            }
        }
        context_ = prev.context;
        contextChanged_ = true;
        for (DataWatch& w : watches_) ReadMemory(w.address, &w.value, w.size);
        stop = new DebugStop{DebugStop::Reason::StepBack, L"", LocationOf(context_.Rip), context_};
    }
    PostMessageW(notify_, WM_DEBUG_STOPPED, 0, reinterpret_cast<LPARAM>(stop));
    return true;
}

// ---- profiler ---------------------------------------------------------------------------------------------------

// Called for every traced instruction: rip is the next instruction to run, the time since the previous call belongs
// to the previous one's line. Code without source (DLLs, the C runtime) runs at full speed until it returns to a
// source line, and its time goes to the line that called it.
int Debugger::ProfileIndex(uint64_t address) {
    if (!InMainImage(address)) return -1;
    auto it = profileIndex_.find(address);
    if (it != profileIndex_.end()) return it->second;
    int index = -1;
    SourceLocation loc = LocationOf(address);
    if (loc.valid()) {
        auto key = std::make_pair(Key(loc.file), loc.line);
        auto byLine = profileByLine_.find(key);
        if (byLine == profileByLine_.end()) {
            byLine = profileByLine_.emplace(key, static_cast<int>(profile_.size())).first;
            profile_.push_back({loc});
        }
        index = byLine->second;
    }
    profileIndex_[address] = index;
    return index;
}

// The sampling profiler: every millisecond the main thread is suspended for a moment and its position counted.
// Time in code without source (printf, the WinAPI) goes to the source line that called it (unwinding by .pdata).
void Debugger::Sampler() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due{};
    due.QuadPart = -10000;  // 1 ms
    SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (sampling_) {
        WaitForSingleObject(timer, 100);
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
        last = now;
        HANDLE thread = mainThread_;
        if (stopped_ || !thread || SuspendThread(thread) == static_cast<DWORD>(-1)) continue;
        alignas(16) CONTEXT ctx;
        bool ok = GetContext(thread, ctx);
        ResumeThread(thread);
        if (!ok) continue;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        uint64_t regs[16] = {ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsp, ctx.Rbp, ctx.Rsi, ctx.Rdi,
                             ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15};
        uint64_t pc = ctx.Rip;
        for (int depth = 0; depth < 24; depth++) {
            int index = ProfileIndex(depth ? pc - 1 : pc);  // a return address: the line of its call
            if (index >= 0) {
                profile_[index].samples++;
                profile_[index].seconds += dt;
                break;
            }
            if (wow64_ || !UnwindFrame(regs, pc, depth > 0)) break;
        }
    }
    CloseHandle(timer);
}

void Debugger::ProfileStep(const CONTEXT& ctx) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (profilePrev_ >= 0) profile_[profilePrev_].seconds += double(now.QuadPart - profileTick_) / freq.QuadPart;
    profileTrace_ = false;
    int index = -1;
    if (!profileLimitHit_) index = ProfileIndex(ctx.Rip);
    if (index >= 0) {
        if (++profileTotal_ >= kProfileLimit) profileLimitHit_ = true;
        profile_[index].instructions++;
        if (index != profilePrev_) profile_[index].hits++;
        profilePrev_ = index;
        profileTrace_ = true;
    } else if (!profileLimitHit_) {
        // Entered code without source by a call: stop on the way back. (After main returns, [rsp] does not lead
        // back to a source line, and the program runs on untraced.)
        uint64_t ret = ReadPointer(ctx.Rsp);
        if (InMainImage(ret) && LocationOf(ret).valid()) Install(ret, false, false, true);
    }
    QueryPerformanceCounter(&now);  // the debugger's own work is not the program's time
    profileTick_ = now.QuadPart;
}

bool Debugger::RunTo(const SourceLocation& where) {
    if (gdb_) return stopped_ && gdb_->RunTo(where);
    auto address = AddressOfLine(where);
    if (!address || !stopped_) return false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!Install(*address, false, false, true)) return false;
        breakpoints_[*address].runTo = true;
    }
    Continue();
    return true;
}

bool Debugger::SetRegister(const std::string& name, uint64_t value) {
    if (!stopped_) return false;
    if (gdb_) return gdb_->SetRegister(name, value);
    if (wow64_) value &= 0xFFFFFFFF;
    if (!SetRegisterIn(context_, name, value)) return false;
    contextChanged_ = true;
    return true;
}

bool Debugger::SetNextStatement(const SourceLocation& where, std::wstring& error) {
    if (!stopped_) return false;
    if (gdb_) return gdb_->SetNextStatement(where, error);
    auto address = AddressOfLine(where);
    if (!address) {
        error = Tr(L"на этой строке нет кода");
        return false;
    }
    if (foreignStop_) {
        error = Tr(L"программа остановлена внутри системного вызова — сделайте шаг, чтобы вернуться в свой код");
        return false;
    }
    context_.Rip = *address;
    contextChanged_ = true;
    return true;
}

bool Debugger::WriteMemory(uint64_t address, const void* data, size_t size) {
    if (gdb_) return stopped_ && gdb_->WriteMemory(address, data, size);
    HANDLE p = process_;
    if (!p || !stopped_) return false;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    for (auto& [addr, bp] : breakpoints_) {  // keep int3 in place: the new byte becomes the "original" one
        if (addr < address || addr >= address + size || addr == reinsertAt_) continue;
        bp.original = bytes[addr - address];
        bytes[addr - address] = 0xCC;
    }
    SIZE_T n = 0;
    if (!WriteProcessMemory(p, reinterpret_cast<void*>(address), bytes.data(), size, &n) || n != size) {
        DWORD old = 0;
        if (!VirtualProtectEx(p, reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old)) return false;
        bool ok = WriteProcessMemory(p, reinterpret_cast<void*>(address), bytes.data(), size, &n) && n == size;
        VirtualProtectEx(p, reinterpret_cast<void*>(address), size, old, &old);
        if (!ok) return false;
    }
    FlushInstructionCache(p, reinterpret_cast<void*>(address), size);
    return true;
}

bool Debugger::AddDataWatch(const DataWatch& watch, std::wstring& error) {
    if (gdb_) return gdb_->AddDataWatch(watch, error);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (watches_.size() >= 4) {
        error = Tr(L"процессор поддерживает не больше 4 точек останова на данные");
        return false;
    }
    if (watch.size != 1 && watch.size != 2 && watch.size != 4 && watch.size != 8) {
        error = Tr(L"размер должен быть 1, 2, 4 или 8 байт");
        return false;
    }
    if (wow64_ && watch.size == 8) {
        error = Tr(L"в 32-битной программе — не больше 4 байт");
        return false;
    }
    if (watch.address % watch.size) {
        error = Tr(L"адрес должен быть кратен размеру: ") + std::to_wstring(watch.size);
        return false;
    }
    DataWatch w = watch;
    w.value = 0;
    ReadMemory(w.address, &w.value, w.size);
    watches_.push_back(w);
    watchesChanged_ = true;
    return true;
}

void Debugger::RemoveDataWatch(size_t index) {
    if (gdb_) return gdb_->RemoveDataWatch(index);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (index < watches_.size()) watches_.erase(watches_.begin() + index);
    watchesChanged_ = true;
}

std::vector<DataWatch> Debugger::DataWatches() {
    if (gdb_) return gdb_->DataWatches();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return watches_;
}

// ---- debug loop (debugger thread) -------------------------------------------------------------------------------

void Debugger::Loop(LaunchOptions launch, HPCON console, Breakpoints bps, bool stopAtEntry,
                    std::optional<SourceLocation> runTo, std::promise<std::wstring>* started) {
    if (launch.attachPid) {  // a running program: its own console stays, Stop detaches and leaves it running
        if (!DebugActiveProcess(launch.attachPid)) {
            started->set_value(Tr(L"не удалось подключиться к процессу, код ") + std::to_wstring(GetLastError()));
            return;
        }
        DebugSetProcessKillOnExit(FALSE);
        attached_ = true;
    } else {
        PROCESS_INFORMATION pi{};
        std::wstring cmd = Toolchain::QuoteCommandLine({launch.exe.wstring()});
        if (!launch.args.empty()) cmd += L" " + launch.args;
        if (!Toolchain::SpawnInConsole(cmd, launch.cwd, console, DEBUG_ONLY_THIS_PROCESS, pi, launch.environment)) {
            started->set_value(Tr(L"не удалось запустить процесс, код ") + std::to_wstring(GetLastError()));
            return;
        }
        CloseHandle(pi.hProcess);  // the debug events provide their own handles
        CloseHandle(pi.hThread);
    }
    startTick_ = GetTickCount64();
    started->set_value(L"");

    DWORD exitCode = 0;
    std::string stats;
    for (bool done = false; !done;) {
        if (detachRequested_) {  // attached: put the original bytes back and let the program go
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                for (const auto& [addr, bp] : breakpoints_) {
                    WriteProcessMemory(process_, reinterpret_cast<void*>(addr), &bp.original, 1, nullptr);
                    FlushInstructionCache(process_, reinterpret_cast<void*>(addr), 1);
                }
                breakpoints_.clear();
            }
            DebugActiveProcessStop(launch.attachPid);
            stats = Tr(std::string("отключено, программа продолжает работу"));
            break;
        }
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
            mainThreadId_ = ev.dwThreadId;
            threads_[ev.dwThreadId] = info.hThread;
            BOOL wow = FALSE;
            wow64_ = IsWow64Process(info.hProcess, &wow) && wow;
            imageBase_ = reinterpret_cast<uint64_t>(info.lpBaseOfImage);
            imageSize_ = ImageSize(imageBase_);
            if (launch.exe.empty() && info.hFile) {  // attached: the program's path (for its .pdb)
                wchar_t path[MAX_PATH * 2] = L"";
                if (GetFinalPathNameByHandleW(info.hFile, path, static_cast<DWORD>(std::size(path)), FILE_NAME_NORMALIZED)) {
                    std::wstring p = path;
                    if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
                    launch.exe = p;
                }
            }
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
                SymInitializeW(process_, launch.exe.parent_path().c_str(), FALSE);
                if (!SymLoadModuleExW(process_, info.hFile, launch.exe.c_str(), nullptr, imageBase_, 0, nullptr, 0)) {
                    PostLog(notify_, Tr(std::string("Не удалось загрузить отладочную информацию\n")), Out::Warning);
                }
                IMAGEHLP_MODULEW64 mod{sizeof(mod)};
                if (SymGetModuleInfoW64(process_, imageBase_, &mod) && !mod.LineNumbers) {
                    PostLog(notify_, Tr(std::string("В программе нет номеров строк: нужны «-g -F cv8» для NASM и /debug для link.\n")),
                        Out::Warning);
                }
                if (wow64_) PostLog(notify_, Tr(std::string("32-битная программа (WOW64)\n")), Out::Info);
                for (const auto& [key, lines] : bps) InstallFile(key, lines);
                FindWritableSections();
                profiling_ = launch.profile;
                if (launch.sample) {
                    sampling_ = true;
                    sampler_ = std::thread(&Debugger::Sampler, this);
                }
                if (stopAtEntry || profiling_) {
                    SymbolBuffer sym;
                    uint64_t entry = reinterpret_cast<uint64_t>(info.lpStartAddress);
                    for (const wchar_t* name : {L"main", L"_main", L"wmain", L"WinMain", L"_WinMain@16"}) {
                        if (SymFromNameW(process_, name, &sym.info)) {
                            entry = sym.info.Address;
                            break;
                        }
                    }
                    // The profiler starts tracing at main (a temporary breakpoint does not stop).
                    Install(entry, false, stopAtEntry, profiling_ && !stopAtEntry);
                }
                if (runTo) {
                    if (auto a = AddressOfLine(*runTo); a && Install(*a, false, false, false)) {
                        breakpoints_[*a].runTo = true;
                        breakpoints_[*a].entry = true;  // survives the stops until it is hit once
                    }
                }
            }
            if (info.hFile) CloseHandle(info.hFile);
            break;
        }
        case CREATE_THREAD_DEBUG_EVENT: {
            HANDLE t = ev.u.CreateThread.hThread;
            threads_[ev.dwThreadId] = t;
            bool watching = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                watching = !watches_.empty();
            }
            alignas(16) CONTEXT ctx;
            if (watching && GetContext(t, ctx)) {  // the new thread reported the event: its context is cheap to set
                FillDebugRegisters(ctx);
                SetContext(t, ctx, false);
            }
            break;
        }
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
            functionTables_.erase(reinterpret_cast<uint64_t>(ev.u.UnloadDll.lpBaseOfDll));
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
            PostMessageW(notify_, WM_DEBUG_ODS, 0, reinterpret_cast<LPARAM>(new std::string(text)));
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
            status = OnException(ev);
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            exitCode = ev.u.ExitProcess.dwExitCode;
            if (sampling_) {
                sampling_ = false;
                if (sampler_.joinable()) sampler_.join();
                PostMessageW(notify_, WM_DEBUG_PROFILE, 0, reinterpret_cast<LPARAM>(new std::vector<ProfileLine>(profile_)));
            }
            if (profiling_) {
                auto* lines = new std::vector<ProfileLine>(profile_);
                if (profileLimitHit_) {
                    PostLog(notify_, Tr(std::string("Профилирование остановлено после 1 млн инструкций — дальше программа работала "
                                                     "без подсчёта.\n")), Out::Warning);
                }
                PostMessageW(notify_, WM_DEBUG_PROFILE, 0, reinterpret_cast<LPARAM>(lines));
            }
            stats = Toolchain::ProcessStats(process_, startTick_);
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
        functionTables_.clear();
    }
    stopped_ = false;
    running_ = false;
    PostMessageW(notify_, WM_DEBUG_EXITED, exitCode, reinterpret_cast<LPARAM>(new std::string(stats)));
}

DWORD Debugger::OnException(const DEBUG_EVENT& ev) {
    const EXCEPTION_RECORD& rec = ev.u.Exception.ExceptionRecord;
    auto threadIt = threads_.find(ev.dwThreadId);
    HANDLE thread = threadIt != threads_.end() ? threadIt->second : mainThread_;
    uint64_t address = reinterpret_cast<uint64_t>(rec.ExceptionAddress);
    alignas(16) CONTEXT ctx;

    switch (rec.ExceptionCode) {
    case EXCEPTION_BREAKPOINT:
    case kStatusWx86Breakpoint: {
        // The loader stops once in ntdll (and, for a 32-bit program, once more in its 32-bit ntdll).
        bool& seen = rec.ExceptionCode == kStatusWx86Breakpoint ? wowLoaderBreakpointSeen_ : loaderBreakpointSeen_;
        if (!seen && (rec.ExceptionCode == kStatusWx86Breakpoint || !wow64_ || !InMainImage(address))) {
            bool ours = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                ours = breakpoints_.count(address) > 0;
            }
            if (!ours) {
                seen = true;
                return DBG_CONTINUE;
            }
        }
        Breakpoint bp;
        bool found = false, wasRemoved = false;
        GetContext(thread, ctx);
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            auto it = breakpoints_.find(address);
            if (it != breakpoints_.end()) {
                found = true;
                bp = it->second;
                // A user breakpoint whose condition is false behaves as if it were not there.
                if (bp.user && !ConditionHolds(it->second, ctx)) bp.user = false;
                if (bp.entry) {
                    it->second.entry = false;
                    if (!it->second.user && !it->second.temp) Remove(address);
                }
            } else {
                wasRemoved = removed_.erase(address) > 0;
            }
        }
        if (!found && pauseRequested_ && !wasRemoved) {  // DebugBreakProcess runs int3 in a helper thread
            pauseRequested_ = false;
            stepMode_ = StepMode::None;
            GetContext(mainThread_, ctx);
            foreignStop_ = true;
            StopAt(mainThread_, ctx, DebugStop::Reason::Pause, L"");
            return DBG_CONTINUE;
        }
        if (!found) {
            if (wasRemoved) {  // hit a breakpoint the user removed a moment ago
                ctx.Rip = address;
                SetContext(thread, ctx, false);
                return DBG_CONTINUE;
            }
            stepMode_ = StepMode::None;
            StopAt(thread, ctx, DebugStop::Reason::Int3, L"");
            return DBG_CONTINUE;
        }
        ctx.Rip = address;  // re-execute the original instruction
        if (bp.user || bp.entry) {
            stepMode_ = StepMode::None;
            DebugStop::Reason reason = bp.runTo && !bp.user ? DebugStop::Reason::RunTo
                                       : bp.entry && !bp.user ? DebugStop::Reason::Entry
                                                              : DebugStop::Reason::Breakpoint;
            std::wstring message;
            if (bp.user && !bp.condition.empty()) message = Tr(L"условие: ") + Widen(bp.condition);
            StopAt(thread, ctx, reason, message);
        } else if (bp.runTo) {
            stepMode_ = StepMode::None;
            StopAt(thread, ctx, DebugStop::Reason::RunTo, L"");
        } else if (pauseRequested_ || stepFromForeign_) {
            bool step = stepFromForeign_;
            pauseRequested_ = false;
            stepFromForeign_ = false;
            stepMode_ = StepMode::None;
            StopAt(thread, ctx, step ? DebugStop::Reason::Step : DebugStop::Reason::Pause, L"");
        } else {
            if (bp.temp) RemoveTemps();  // the return address of a stepped-over call
            if (profiling_ && stepMode_ == StepMode::None) ProfileStep(ctx);
            if (stepMode_ == StepMode::Out) ContinueStepping(thread, ctx);
            else if (stepMode_ != StepMode::None) CheckStop(thread, ctx);
            else RunFree(thread, ctx);
        }
        return DBG_CONTINUE;
    }
    case EXCEPTION_SINGLE_STEP:
    case kStatusWx86SingleStep: {
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
        if (ctx.Dr6 & 0xF) {  // a data breakpoint fired (after the write)
            std::wstring message = WatchHit(ctx.Dr6);
            ctx.Dr6 = 0;
            if (!message.empty()) {
                stepMode_ = StepMode::None;
                StopAt(thread, ctx, DebugStop::Reason::Watchpoint, message);
                return DBG_CONTINUE;
            }
        }
        if (stepMode_ == StepMode::None) {
            if (profiling_) ProfileStep(ctx);
            Resume(thread, ctx, false);
        } else {
            OnStep(thread, ctx);
        }
        return DBG_CONTINUE;
    }
    default: {
        if ((rec.ExceptionCode & 0xF0000000) != 0xC0000000) return DBG_EXCEPTION_NOT_HANDLED;  // informational
        GetContext(thread, ctx);
        stepMode_ = StepMode::None;
        std::wstring msg = std::wstring(ev.u.Exception.dwFirstChance ? Tr(L"Исключение ") : Tr(L"Необработанное исключение ")) +
                           Toolchain::DescribeExitCode(rec.ExceptionCode);
        if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
            wchar_t buf[96];
            const wchar_t* kind = rec.ExceptionInformation[0] == 0 ? Tr(L"чтение") : rec.ExceptionInformation[0] == 1 ? Tr(L"запись") : Tr(L"выполнение");
            swprintf_s(buf, Tr(L"; %s по адресу 0x%llX"), kind, static_cast<unsigned long long>(rec.ExceptionInformation[1]));
            msg += buf;
        }
        StopAt(thread, ctx, DebugStop::Reason::Exception, msg);
        return DBG_EXCEPTION_NOT_HANDLED;
    }
    }
}

// A user breakpoint's condition: "#N" stops on the N-th hit, an expression stops when it is not 0.
// "log: text {expr} {expr:x}" (after an optional condition) makes a logpoint: it prints instead of stopping.
bool Debugger::ConditionHolds(Breakpoint& bp, const CONTEXT& ctx) {
    bp.hits++;
    std::string cond = bp.condition, message;
    bool logpoint = false;
    if (size_t log = cond.find("log:"); log != std::string::npos) {
        logpoint = true;
        message = cond.substr(log + 4);
        if (!message.empty() && message[0] == ' ') message.erase(0, 1);
        cond.erase(log);
    }
    size_t first = cond.find_first_not_of(" \t");
    bool holds = true;
    alignas(16) CONTEXT saved = context_;
    context_ = ctx;
    if (first != std::string::npos) {
        cond = cond.substr(first);
        if (cond[0] == '#') {
            size_t end = 1;
            while (end < cond.size() && isdigit(static_cast<unsigned char>(cond[end]))) end++;
            int target = atoi(cond.substr(1, end - 1).c_str());
            if (target > 0 && bp.hits != target) holds = false;
            cond = cond.substr(end);
        }
        if (holds && cond.find_first_not_of(" \t") != std::string::npos) {
            std::wstring error;
            auto v = Evaluate(cond, error);
            if (!v) {
                PostLog(notify_, Tr(std::string("Условие точки останова не вычисляется, остановка: ")) + bp.condition + ": " +
                                     Narrow(error) + "\n",
                        Out::Warning);
                context_ = saved;
                return true;
            }
            holds = *v != 0;
        }
    }
    if (holds && logpoint) {
        PostMessageW(notify_, WM_DEBUG_ODS, 0, reinterpret_cast<LPARAM>(new std::string(FormatLogMessage(message) + "\n")));
        holds = false;
    }
    context_ = saved;
    return holds;
}

// Replaces {expr} with its value (decimal, signed) and {expr:x} with hex; {{ and }} are literal braces.
std::string Debugger::FormatLogMessage(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if ((ch == '{' || ch == '}') && i + 1 < text.size() && text[i + 1] == ch) {
            out += ch;
            i++;
            continue;
        }
        size_t close = ch == '{' ? text.find('}', i) : std::string::npos;
        if (close == std::string::npos) {
            out += ch;
            continue;
        }
        std::string expr = text.substr(i + 1, close - i - 1);
        bool hex = false;
        if (expr.size() > 2 && (expr.substr(expr.size() - 2) == ":x" || expr.substr(expr.size() - 2) == ":X")) {
            hex = true;
            expr.resize(expr.size() - 2);
        }
        std::wstring error;
        auto v = Evaluate(expr, error);
        char buf[40];
        if (!v) out += "<" + Narrow(error) + ">";
        else if (hex) sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(*v)), out += buf;
        else sprintf_s(buf, "%lld", static_cast<long long>(*v)), out += buf;
        i = close;
    }
    return out;
}

// Which watched variables changed (DR6 bits 0-3 name the debug registers that fired).
std::wstring Debugger::WatchHit(uint64_t dr6) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::wstring message;
    for (size_t i = 0; i < watches_.size(); i++) {
        if (!(dr6 & (1ull << i))) continue;
        DataWatch& w = watches_[i];
        uint64_t now = 0;
        ReadMemory(w.address, &now, w.size);
        wchar_t buf[160];
        swprintf_s(buf, Tr(L"%s изменилось: 0x%llX → 0x%llX (%lld)"), Widen(w.expression).c_str(),
                   static_cast<unsigned long long>(w.value), static_cast<unsigned long long>(now),
                   static_cast<long long>(now));
        if (!message.empty()) message += L"; ";
        message += buf;
        w.value = now;
    }
    return message;
}

// Called after every single-stepped instruction while a step command is active.
void Debugger::OnStep(HANDLE thread, CONTEXT& ctx) {
    // Detect "call": rsp dropped by a pointer and the pushed value points just past the previous instruction.
    int ptr = PointerSize();
    uint64_t pushed = ctx.Rsp == prevRsp_ - ptr ? ReadPointer(ctx.Rsp) : 0;
    bool isCall = pushed > prevRip_ && pushed <= prevRip_ + 15 && ctx.Rip != pushed;
    bool isRet = ctx.Rsp > prevRsp_ && ReadPointer(prevRsp_) == ctx.Rip;

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
    stopThread_ = thread;
    if (!foreignStop_) RecordSnapshot(thread, ctx);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (DataWatch& w : watches_) ReadMemory(w.address, &w.value, w.size);
    }
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
    if (contextChanged_) ctx = context_;  // registers or rip edited in the panel
    stopped_ = false;

    if (foreignStop_) {
        // Stopped through DebugBreakProcess: the main thread is inside a system call and did not report the event,
        // and changing its context from here would block for seconds. Leave it alone: a step continues until it
        // comes back to a source line, "continue" just lets it run.
        foreignStop_ = false;
        contextChanged_ = false;
        if (cmd == Command::Stop) {
            if (!attached_) TerminateProcess(process_, 1);
        } else if (cmd != Command::Continue) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            for (uint64_t a : lineAddresses_) Install(a, false, false, true);
            stepFromForeign_ = true;
        }
        return;
    }
    switch (cmd) {
    case Command::StepInto:
    case Command::StepOver:
    case Command::StepOut:
        stepMode_ = cmd == Command::StepInto ? StepMode::Into : cmd == Command::StepOver ? StepMode::Over : StepMode::Out;
        stepFrom_ = LocationOf(ctx.Rip);
        ContinueStepping(thread, ctx);
        break;
    case Command::Stop:
        if (attached_) RunFree(thread, ctx);  // detached at the top of the debug loop
        else TerminateProcess(process_, 1);
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
        FillDebugRegisters(ctx);
        if (watchesChanged_) ApplyWatchesToOtherThreads(thread, ctx);
        watchesChanged_ = false;
    }
    if (profiling_ && profileTrace_ && !profileLimitHit_) singleStep = true;  // trace the source lines
    if (singleStep) ctx.EFlags |= kTrapFlag;
    else ctx.EFlags &= ~kTrapFlag;
    SetContext(thread, ctx, contextChanged_);
    contextChanged_ = false;
}

// Data breakpoints live in the debug registers of each thread (DR7: enable, write-only, length).
void Debugger::FillDebugRegisters(CONTEXT& ctx) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
    ctx.Dr6 = 0;
    ctx.Dr7 = 0;
    DWORD64* dr[] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
    for (size_t i = 0; i < watches_.size() && i < 4; i++) {
        const DataWatch& w = watches_[i];
        uint64_t len = w.size == 1 ? 0 : w.size == 2 ? 1 : w.size == 8 ? 2 : 3;
        *dr[i] = w.address;
        ctx.Dr7 |= (1ull << (i * 2)) | (1ull << (16 + i * 4)) | (len << (18 + i * 4));
    }
}

// The other threads did not report the event, and setting their context blocks until they run again (a kernel
// APC), so it is done from a helper thread after the program continues. Debug registers only, nothing else.
void Debugger::ApplyWatchesToOtherThreads(HANDLE current, const CONTEXT& ctx) {
    std::vector<HANDLE> others;
    for (const auto& [id, h] : threads_) {
        HANDLE dup = nullptr;
        if (h != current && DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE,
                                            DUPLICATE_SAME_ACCESS)) {
            others.push_back(dup);
        }
    }
    if (others.empty()) return;
    DWORD64 dr[5] = {ctx.Dr0, ctx.Dr1, ctx.Dr2, ctx.Dr3, ctx.Dr7};
    bool wow = wow64_;
    std::thread([others, dr, wow] {
        for (HANDLE h : others) {
            if (wow) {
                WOW64_CONTEXT w{};
                w.ContextFlags = WOW64_CONTEXT_DEBUG_REGISTERS;
                w.Dr0 = static_cast<DWORD>(dr[0]);
                w.Dr1 = static_cast<DWORD>(dr[1]);
                w.Dr2 = static_cast<DWORD>(dr[2]);
                w.Dr3 = static_cast<DWORD>(dr[3]);
                w.Dr7 = static_cast<DWORD>(dr[4]);
                Wow64SetThreadContext(h, &w);
            } else {
                alignas(16) CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                c.Dr0 = dr[0];
                c.Dr1 = dr[1];
                c.Dr2 = dr[2];
                c.Dr3 = dr[3];
                c.Dr7 = dr[4];
                SetThreadContext(h, &c);
            }
            CloseHandle(h);
        }
    }).detach();
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
    Breakpoint bp;
    bp.address = address;
    bp.user = user;
    bp.entry = entry;
    bp.temp = temp;
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
        if (bp.runTo && bp.entry) continue;  // "run to" given at start: waits for its own hit
        if (!bp.temp && !bp.runTo) continue;
        bp.temp = false;
        bp.runTo = false;
        if (!bp.user && !bp.entry) drop.push_back(addr);
    }
    for (uint64_t a : drop) Remove(a);
}

std::optional<uint64_t> Debugger::AddressOfLine(const SourceLocation& where) {
    if (gdb_) return gdb_->AddressOfLine(where);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!process_) return std::nullopt;
    LONG displacement = 0;
    IMAGEHLP_LINEW64 info{sizeof(info)};
    std::wstring key = Key(where.file);
    if (!SymGetLineFromNameW64(process_, nullptr, key.c_str(), where.line, &displacement, &info)) return std::nullopt;
    if (!SamePath(info.FileName, key) || static_cast<int>(info.LineNumber) < where.line ||
        static_cast<int>(info.LineNumber) > where.line + 20) {
        return std::nullopt;  // no code on or shortly after this line
    }
    return info.Address;
}

void Debugger::InstallFile(const std::wstring& key, const std::map<int, std::string>& lines) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto& addrs = fileBreakpoints_[key];
    for (const auto& [line, condition] : lines) {
        auto address = AddressOfLine({key, line});
        if (!address || !Install(*address, true, false, false)) continue;
        breakpoints_[*address].condition = condition;
        breakpoints_[*address].hits = 0;
        addrs.push_back(*address);
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
        bp->second.condition.clear();
        if (!bp->second.entry && !bp->second.temp && !bp->second.runTo) Remove(addr);
    }
    fileBreakpoints_.erase(it);
}

// ---- inspection -------------------------------------------------------------------------------------------------

uint64_t Debugger::ImageSize(uint64_t base) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (ReadMemory(base, &dos, sizeof(dos)) != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0x1000;
    if (ReadMemory(base + dos.e_lfanew, &nt, sizeof(nt)) < sizeof(IMAGE_NT_HEADERS32) || nt.Signature != IMAGE_NT_SIGNATURE) {
        return 0x1000;
    }
    if (nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return reinterpret_cast<const IMAGE_NT_HEADERS32*>(&nt)->OptionalHeader.SizeOfImage;
    }
    return nt.OptionalHeader.SizeOfImage;
}

SourceLocation Debugger::LocationOf(uint64_t address) {
    if (gdb_) return gdb_->LocationOf(address);
    if (!InMainImage(address)) return {};  // source exists only for our own executable
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    DWORD displacement = 0;
    IMAGEHLP_LINEW64 line{sizeof(line)};
    if (process_ && SymGetLineFromAddrW64(process_, address, &displacement, &line) && FileExists(line.FileName)) {
        return {NormalizePath(line.FileName), static_cast<int>(line.LineNumber)};
    }
    return {};
}

uint64_t Debugger::ReadPointer(uint64_t address) {
    uint64_t v = 0;
    SIZE_T n = 0;
    int size = PointerSize();
    ReadProcessMemory(process_, reinterpret_cast<void*>(address), &v, size, &n);
    return n == static_cast<SIZE_T>(size) ? v : 0;
}

size_t Debugger::ReadMemory(uint64_t address, void* buffer, size_t size) {
    if (gdb_) return gdb_->ReadMemory(address, buffer, size);
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
    if (gdb_) return gdb_->LookupSymbol(name);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!process_) return std::nullopt;
    SymbolBuffer sym;
    if (SymFromNameW(process_, Widen(name).c_str(), &sym.info)) return sym.info.Address;
    if (wow64_ && SymFromNameW(process_, (L"_" + Widen(name)).c_str(), &sym.info)) return sym.info.Address;
    return std::nullopt;
}

std::wstring Debugger::SymbolAt(uint64_t address) {
    if (gdb_) return gdb_->SymbolAt(address);
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

// A value on the stack that points just past a call instruction in some loaded module.
bool Debugger::LooksLikeReturnAddress(uint64_t value) {
    if (value < 0x10000) return false;
    bool known = InMainImage(value);
    if (!known) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = modules_.upper_bound(value);
        if (it != modules_.begin()) {
            --it;
            known = value < it->second.base + it->second.size;
        }
    }
    if (!known) return false;
    uint8_t before[8]{};
    if (ReadMemory(value - 8, before, 8) != 8) return false;
    if (before[3] == 0xE8) return true;  // call rel32
    return EndsWithIndirectCall(before, 8, wow64_);
}

uint64_t Debugger::ModuleBase(uint64_t address) {
    if (InMainImage(address)) return imageBase_;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = modules_.upper_bound(address);
    if (it == modules_.begin()) return 0;
    --it;
    return address < it->second.base + it->second.size ? it->second.base : 0;
}

// The x64 exception directory (.pdata) of a loaded module, read from the program's memory once.
const std::vector<Debugger::RuntimeFunction>& Debugger::FunctionTable(uint64_t base) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = functionTables_.find(base);
    if (it != functionTables_.end()) return it->second;
    std::vector<RuntimeFunction>& table = functionTables_[base];
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (ReadMemory(base, &dos, sizeof(dos)) != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        ReadMemory(base + dos.e_lfanew, &nt, sizeof(nt)) != sizeof(nt) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
        return table;
    }
    const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    size_t count = std::min<size_t>(dir.Size / sizeof(RuntimeFunction), 1 << 20);
    table.resize(count);
    size_t got = ReadMemory(base + dir.VirtualAddress, table.data(), count * sizeof(RuntimeFunction));
    table.resize(got / sizeof(RuntimeFunction));
    return table;
}

// One frame of x64 unwinding from the module's unwind information (what RtlVirtualUnwind does, but on another
// process's memory). regs are in unwind-code order: rax rcx rdx rbx rsp rbp rsi rdi r8-r15. False: no unwind
// information for this address (NASM code, or a leaf function).
bool Debugger::UnwindFrame(uint64_t regs[16], uint64_t& rip, bool caller) {
    uint64_t base = ModuleBase(rip);
    if (!base) return false;
    const auto& table = FunctionTable(base);
    uint32_t rva = static_cast<uint32_t>(rip - base - (caller ? 1 : 0));
    auto it = std::upper_bound(table.begin(), table.end(), rva,
                               [](uint32_t v, const RuntimeFunction& f) { return v < f.begin; });
    if (it == table.begin()) return false;
    --it;
    if (rva >= it->end) return false;
    auto read64 = [&](uint64_t a) {
        uint64_t v = 0;
        ReadMemory(a, &v, 8);
        return v;
    };
    uint64_t& rsp = regs[4];
    uint64_t offset = rip - (base + it->begin);

    // In an epilog (add/lea rsp, pops, ret) the prolog's work is partly undone: emulate the rest of the epilog.
    uint8_t code[48]{};
    size_t n = ReadMemory(rip, code, sizeof(code));
    if (offset > 0 && n >= 1) {
        size_t i = 0;
        uint64_t sp = rsp;
        bool ok = true;
        if (n > 4 && code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xC4) {  // add rsp, imm8
            sp += static_cast<int8_t>(code[3]);
            i = 4;
        } else if (n > 7 && code[0] == 0x48 && code[1] == 0x81 && code[2] == 0xC4) {  // add rsp, imm32
            int32_t imm;
            memcpy(&imm, code + 3, 4);
            sp += imm;
            i = 7;
        } else if (n > 4 && (code[0] & 0xFE) == 0x48 && code[1] == 0x8D && (code[2] & 0x38) == 0x20) {  // lea rsp, [r+d]
            int reg = (code[2] & 7) + ((code[0] & 1) ? 8 : 0);
            int mod = code[2] >> 6;
            size_t at = 3;
            if ((code[2] & 7) == 4) at++;  // SIB (rsp-based)
            int64_t disp = 0;
            if (mod == 1) disp = static_cast<int8_t>(code[at++]);
            else if (mod == 2) {
                int32_t d32;
                memcpy(&d32, code + at, 4);
                disp = d32;
                at += 4;
            } else ok = false;
            sp = regs[reg] + disp;
            i = at;
        }
        std::vector<int> pops;
        while (ok && i < n) {
            if (code[i] >= 0x58 && code[i] <= 0x5F) {
                pops.push_back(code[i] - 0x58);
                i++;
            } else if (code[i] == 0x41 && i + 1 < n && code[i + 1] >= 0x58 && code[i + 1] <= 0x5F) {
                pops.push_back(code[i + 1] - 0x58 + 8);
                i += 2;
            } else {
                break;
            }
        }
        if (ok && i < n && code[i] == 0xC3) {
            for (int r : pops) {
                regs[r] = read64(sp);
                sp += 8;
            }
            rip = read64(sp);
            rsp = sp + 8;
            return true;
        }
    }

    bool machineFrame = false;
    uint32_t unwind = it->unwind;
    bool chained = false;
    for (int depth = 0; depth < 32; depth++) {
        uint8_t head[4]{};
        if (ReadMemory(base + unwind, head, 4) != 4) return false;
        int version = head[0] & 7, flags = head[0] >> 3, count = head[2];
        int frameReg = head[3] & 0xF, frameOffset = head[3] >> 4;
        std::vector<uint16_t> codes(count + 2);
        ReadMemory(base + unwind + 4, codes.data(), codes.size() * 2);
        for (int i = 0; i < count;) {
            int at = codes[i] & 0xFF, op = (codes[i] >> 8) & 0xF, info = codes[i] >> 12;
            bool apply = chained || static_cast<uint64_t>(at) <= offset;
            int slots = 1;
            switch (op) {
            case 0:  // UWOP_PUSH_NONVOL
                if (apply) {
                    regs[info] = read64(rsp);
                    rsp += 8;
                }
                break;
            case 1:  // UWOP_ALLOC_LARGE
                if (info == 0) {
                    slots = 2;
                    if (apply) rsp += codes[i + 1] * 8ull;
                } else {
                    slots = 3;
                    if (apply) rsp += codes[i + 1] | (static_cast<uint64_t>(codes[i + 2]) << 16);
                }
                break;
            case 2:  // UWOP_ALLOC_SMALL
                if (apply) rsp += info * 8ull + 8;
                break;
            case 3:  // UWOP_SET_FPREG
                if (apply) rsp = regs[frameReg] - frameOffset * 16ull;
                break;
            case 4:  // UWOP_SAVE_NONVOL
                slots = 2;
                if (apply) regs[info] = read64(rsp + codes[i + 1] * 8ull);
                break;
            case 5:  // UWOP_SAVE_NONVOL_FAR
                slots = 3;
                if (apply) regs[info] = read64(rsp + (codes[i + 1] | (static_cast<uint64_t>(codes[i + 2]) << 16)));
                break;
            case 6:  // v2: UWOP_EPILOG; v1: UWOP_SAVE_XMM
                slots = version == 2 ? 1 : 2;
                break;
            case 7:  // UWOP_SPARE_CODE / UWOP_SAVE_XMM_FAR
                slots = version == 2 ? 2 : 3;
                break;
            case 8:  // UWOP_SAVE_XMM128
                slots = 2;
                break;
            case 9:  // UWOP_SAVE_XMM128_FAR
                slots = 3;
                break;
            case 10:  // UWOP_PUSH_MACHFRAME (interrupt or exception frame)
                if (apply) {
                    uint64_t sp = rsp + (info ? 8 : 0);
                    rip = read64(sp);
                    rsp = read64(sp + 24);
                    machineFrame = true;
                }
                break;
            default:
                return false;
            }
            i += slots;
        }
        if (!(flags & 0x4)) break;  // UNW_FLAG_CHAININFO
        RuntimeFunction parent{};
        if (ReadMemory(base + unwind + 4 + ((count + 1) & ~1) * 2, &parent, sizeof(parent)) != sizeof(parent)) break;
        unwind = parent.unwind;
        chained = true;
    }
    if (!machineFrame) {
        rip = read64(rsp);
        rsp += 8;
    }
    return true;
}

// Call stack. Code with unwind information (the C runtime, Windows DLLs) is unwound exactly; NASM code has none,
// so for its frames the stack is scanned for the nearest return address whose call instruction (when it is a
// direct call) targets the function of the frame above, which skips stale addresses left by earlier calls.
std::vector<Debugger::Frame> Debugger::CallStack() {
    std::vector<Frame> frames;
    if (!stopped_) return frames;
    if (gdb_) return gdb_->CallStack();
    auto globalPart = [](std::wstring name) {
        size_t plus = name.find(L'+');
        if (plus != std::wstring::npos) name.erase(plus);
        size_t dot = name.find(L'.', 1);
        if (dot != std::wstring::npos) name.erase(dot);
        return name;
    };
    Frame top;
    top.pc = context_.Rip;
    top.location = LocationOf(top.pc);
    top.function = SymbolAt(top.pc);
    frames.push_back(top);

    const CONTEXT& c = context_;
    uint64_t regs[16] = {c.Rax, c.Rcx, c.Rdx, c.Rbx, c.Rsp, c.Rbp, c.Rsi, c.Rdi,
                         c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15};
    uint64_t pc = c.Rip;
    int ptr = PointerSize();
    while (frames.size() < 64) {
        uint64_t sp = regs[4], next = 0, slot = 0;
        if (!wow64_ && UnwindFrame(regs, pc, frames.size() > 1)) {
            next = pc;
            slot = regs[4] - 8;
        } else {
            std::vector<uint8_t> stack(ptr * 1024);
            size_t got = ReadMemory(sp, stack.data(), stack.size());
            for (size_t off = 0; off + ptr <= got; off += ptr) {
                uint64_t value = 0;
                memcpy(&value, stack.data() + off, ptr);
                if (!LooksLikeReturnAddress(value)) continue;
                uint8_t call[5]{};
                if (ReadMemory(value - 5, call, 5) == 5 && call[0] == 0xE8) {  // direct call: check its target
                    int32_t rel;
                    memcpy(&rel, call + 1, 4);
                    uint64_t target = value + static_cast<int64_t>(rel);
                    if (ptr == 4) target &= 0xFFFFFFFF;
                    const Frame& callee = frames.back();
                    std::wstring calleeName = globalPart(callee.function);
                    std::wstring targetName = globalPart(SymbolAt(target));
                    if (InMainImage(callee.pc) && InMainImage(target) && !calleeName.empty() && calleeName != targetName) {
                        continue;
                    }
                }
                next = value;
                slot = sp + off;
                break;
            }
            pc = next;
            regs[4] = slot + ptr;
        }
        if (next < 0x10000 || !ModuleBase(next) || regs[4] <= sp) break;
        Frame f;
        f.pc = next;
        f.returnSlot = slot;
        f.location = LocationOf(next - 1);  // the call instruction's line
        f.function = SymbolAt(next);
        frames.push_back(f);
    }
    return frames;
}
