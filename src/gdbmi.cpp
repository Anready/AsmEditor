#include "gdbmi.h"

#include <regex>

// ---- MI records -------------------------------------------------------------------------------------------------

namespace Mi {
namespace {

std::string CString(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    for (i++; i < s.size() && s[i] != '"'; i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        char e = s[++i];
        if (e == 'n') out += '\n';
        else if (e == 't') out += '\t';
        else if (e == 'r') out += '\r';
        else if (e >= '0' && e <= '7') {  // \ooo
            int v = 0, n = 0;
            while (n < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7') v = v * 8 + (s[i++] - '0'), n++;
            i--;
            out += static_cast<char>(v);
        } else out += e;
    }
    i++;  // closing quote
    return out;
}

Value ParseValue(const std::string& s, size_t& i);

void ParseResults(const std::string& s, size_t& i, Value& into, char close) {
    while (i < s.size() && s[i] != close) {
        if (s[i] == ',') {
            i++;
            continue;
        }
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        std::string name = s.substr(i, eq - i);
        i = eq + 1;
        into.fields.push_back({name, ParseValue(s, i)});
    }
}

Value ParseValue(const std::string& s, size_t& i) {
    Value v;
    if (i >= s.size()) return v;
    if (s[i] == '"') {
        v.text = CString(s, i);
        return v;
    }
    char open = s[i];
    if (open != '{' && open != '[') return v;
    char close = open == '{' ? '}' : ']';
    v.kind = open == '{' ? Value::Kind::Tuple : Value::Kind::List;
    i++;
    if (open == '[' && i < s.size() && (s[i] == '"' || s[i] == '{' || s[i] == '[')) {
        while (i < s.size() && s[i] != ']') {
            if (s[i] == ',') {
                i++;
                continue;
            }
            v.items.push_back(ParseValue(s, i));
        }
    } else {
        ParseResults(s, i, v, close);
    }
    if (i < s.size()) i++;
    return v;
}
}

const Value* Value::Get(const std::string& name) const {
    for (const auto& [n, v] : fields) {
        if (n == name) return &v;
    }
    return nullptr;
}

std::string Value::Str(const std::string& name) const {
    const Value* v = Get(name);
    return v && v->kind == Kind::String ? v->text : std::string();
}

std::optional<Record> Parse(const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("(gdb)", 0) == 0) return std::nullopt;
    size_t i = 0;
    Record r;
    while (i < line.size() && isdigit(static_cast<unsigned char>(line[i]))) i++;
    if (i) r.token = atoi(line.substr(0, i).c_str());
    if (i >= line.size()) return std::nullopt;
    char c = line[i];
    if (c == '~' || c == '@' || c == '&') {
        if (r.token >= 0) return std::nullopt;
        r.type = c;
        i++;
        r.text = CString(line, i);
        return r;
    }
    if (c != '^' && c != '*' && c != '+' && c != '=') return std::nullopt;
    r.type = c;
    i++;
    size_t comma = line.find(',', i);
    r.klass = line.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
    r.results.kind = Value::Kind::Tuple;
    if (comma != std::string::npos) {
        i = comma + 1;
        ParseResults(line, i, r.results, '\0');
    }
    return r;
}

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

fs::path FromWsl(const std::string& p) {
    if (p.size() >= 6 && p.rfind("/mnt/", 0) == 0 && isalpha(static_cast<unsigned char>(p[5])) && (p.size() == 6 || p[6] == '/')) {
        std::string rest = p.size() > 7 ? p.substr(7) : "";
        std::replace(rest.begin(), rest.end(), '/', '\\');
        return fs::path(Widen(std::string(1, static_cast<char>(toupper(p[5]))) + ":\\" + rest));
    }
    return fs::path(Widen(p));
}

std::string GdbExpression(const std::string& expr) {
    // Memory operands first: [x] and byte/word/dword/qword [x].
    std::string s;
    for (size_t i = 0; i < expr.size(); i++) {
        if (expr[i] != '[') {
            s += expr[i];
            continue;
        }
        int depth = 1;
        size_t j = i + 1;
        while (j < expr.size() && depth) {
            if (expr[j] == '[') depth++;
            if (expr[j] == ']') depth--;
            j++;
        }
        std::string inner = GdbExpression(expr.substr(i + 1, j - i - 2));
        std::string type = "long";
        static const std::pair<const char*, const char*> sizes[] = {
            {"byte", "unsigned char"}, {"dword", "unsigned int"}, {"qword", "long"}, {"word", "unsigned short"}};
        std::string lower = s;
        for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        size_t end = lower.find_last_not_of(" \t");
        for (auto [kw, t] : sizes) {
            size_t n = strlen(kw);
            if (end != std::string::npos && end + 1 >= n && lower.compare(end + 1 - n, n, kw) == 0 &&
                (end + 1 == n || !isalnum(static_cast<unsigned char>(lower[end - n])))) {
                type = t;
                s.erase(end + 1 - n);
                break;
            }
        }
        s += "*(" + type + "*)(" + inner + ")";
        i = j - 1;
    }
    static const std::regex reg("(^|[^$\\w])(r[a-ds][xip]|r[sd]i|r[sb]p|r(?:8|9|1[0-5])[dwb]?|e[a-ds][xip]|e[sd]i|e[sb]p|"
                                "[a-d]x|[a-d]l|[a-d]h|eflags|rflags)\\b");
    return std::regex_replace(s, reg, "$1$$$2");
}

std::pair<std::string, std::vector<std::string>> LogFormat(const std::string& text) {
    std::string format;
    std::vector<std::string> args;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            format += c;
            i++;
            continue;
        }
        size_t close = c == '{' ? text.find('}', i) : std::string::npos;
        if (close == std::string::npos) {
            format += c == '%' ? std::string("%%") : std::string(1, c);
            continue;
        }
        std::string expr = text.substr(i + 1, close - i - 1);
        bool hex = expr.size() > 2 && (expr.substr(expr.size() - 2) == ":x" || expr.substr(expr.size() - 2) == ":X");
        if (hex) expr.resize(expr.size() - 2);
        format += hex ? "%#lx" : "%ld";
        args.push_back("(long)(" + GdbExpression(expr) + ")");
        i = close;
    }
    return {format, args};
}

uint64_t ParseNumber(const std::string& s) {
    size_t hex = s.find("0x");
    size_t dec = s.find_first_of("0123456789");
    if (hex != std::string::npos && hex == dec) return strtoull(s.c_str() + hex, nullptr, 16);
    if (dec == std::string::npos) return 0;
    bool neg = dec > 0 && s[dec - 1] == '-';
    uint64_t v = strtoull(s.c_str() + dec, nullptr, 10);
    return neg ? 0 - v : v;
}

}

// ---- session ----------------------------------------------------------------------------------------------------

namespace {
std::string Hex(uint64_t v) {
    char buf[24];
    sprintf_s(buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}
std::string BaseName(const fs::path& p) {
    return Narrow(p.filename().wstring());
}
}

GdbSession::GdbSession(Debugger& owner, HWND notify) : d_(owner), notify_(notify) {}

GdbSession::~GdbSession() {
    Stop();
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
    }
    cv_.notify_all();
    if (controller_.joinable()) controller_.join();
    for (HANDLE h : {in_, out_, process_}) {
        if (h) CloseHandle(h);
    }
}

bool GdbSession::Start(const LaunchOptions& launch, const std::string& tty, const Debugger::Breakpoints& breakpoints,
                       bool stopAtEntry, std::optional<SourceLocation> runTo, std::wstring& error) {
    dir_ = launch.cwd;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead, outWrite;
    CreatePipe(&inRead, &in_, &sa, 0);
    CreatePipe(&out_, &outWrite, &sa, 0);
    SetHandleInformation(in_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Toolchain::QuoteCommandLine({L"wsl.exe", L"--cd", Toolchain::WslPath(launch.cwd), L"--exec", L"gdb",
                                                    L"--interpreter=mi2", L"-q", L"-nx", Toolchain::WslPath(launch.exe)});
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, launch.cwd.c_str(), &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
        error = Tr(L"не удалось запустить wsl.exe, код ") + std::to_wstring(GetLastError());
        return false;
    }
    process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    reader_ = std::thread(&GdbSession::Reader, this);
    controller_ = std::thread(&GdbSession::Controller, this);

    if (!Command("-gdb-set confirm off", 20000)) {
        error = Tr(L"gdb не отвечает: установите его в WSL (sudo apt install gdb)");
        return false;
    }
    Command("-gdb-set mi-async on");
    Command("-gdb-set disassembly-flavor intel");
    Command("-gdb-set dprintf-style gdb");
    if (!tty.empty()) Command("-inferior-tty-set " + tty);
    if (!launch.args.empty()) Command("-exec-arguments " + Narrow(launch.args));
    for (const auto& [key, lines] : breakpoints) InsertBreakpoints(key, lines);
    if (runTo) {
        sources_[Lower(runTo->file.filename().wstring())] = runTo->file;
        Command("-break-insert -t " + Mi::Quote(BaseName(runTo->file) + ":" + std::to_string(runTo->line)));
    }
    auto r = Command(stopAtEntry ? "-exec-run --start" : "-exec-run");
    if (!r || r->klass == "error") {
        error = L"gdb: " + Widen(r ? r->results.Str("msg") : Narrow(Tr(L"нет ответа")));
        return false;
    }
    return true;
}

std::optional<Mi::Record> GdbSession::Command(const std::string& command, DWORD timeoutMs) {
    std::lock_guard<std::mutex> one(commandMutex_);
    int token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_) return std::nullopt;
        token = nextToken_++;
    }
    std::string line = std::to_string(token) + command + "\n";
    DWORD n = 0;
    if (!WriteFile(in_, line.data(), static_cast<DWORD>(line.size()), &n, nullptr)) return std::nullopt;
    std::unique_lock<std::mutex> lock(mutex_);
    bool got = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return results_.count(token) || done_; });
    if (!got || !results_.count(token)) return std::nullopt;
    Mi::Record r = std::move(results_[token]);
    results_.erase(token);
    return r;
}

std::string GdbSession::Console(const std::string& command) {
    // The CLI output arrives as ~"..." stream records before the ^done; they are collected by the reader.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        consoleCapture_.clear();
        capturing_ = true;
    }
    Command("-interpreter-exec console " + Mi::Quote(command));
    std::lock_guard<std::mutex> lock(mutex_);
    capturing_ = false;
    return consoleCapture_;
}

void GdbSession::Reader() {
    std::string pending;
    char buf[4096];
    DWORD n;
    while (ReadFile(out_, buf, sizeof(buf), &n, nullptr) && n) {
        pending.append(buf, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            auto r = Mi::Parse(line);
            if (!r) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            if (r->type == '^' && r->token >= 0) {
                results_[r->token] = std::move(*r);
            } else if (r->type == '~' && capturing_) {
                consoleCapture_ += r->text;
            } else {
                events_.push_back(std::move(*r));
            }
            cv_.notify_all();
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    cv_.notify_all();
}

void GdbSession::Controller() {
    bool exited = false;
    for (;;) {
        Mi::Record r;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return !events_.empty() || done_; });
            if (events_.empty()) break;
            r = std::move(events_.front());
            events_.pop_front();
        }
        if (r.type == '*' && r.klass == "running") {
            d_.stopped_ = false;
        } else if (r.type == '*' && r.klass == "stopped") {
            std::string reason = r.results.Str("reason");
            if (reason.rfind("exited", 0) == 0) {
                DWORD code = reason == "exited" ? static_cast<DWORD>(strtoul(r.results.Str("exit-code").c_str(), nullptr, 8))
                           : reason == "exited-signalled" ? 0xC0000005 : 0;
                exited = true;
                d_.stopped_ = false;
                d_.running_ = false;
                PostMessageW(notify_, WM_DEBUG_EXITED, code, reinterpret_cast<LPARAM>(new std::string()));
                DWORD w;
                WriteFile(in_, "-gdb-exit\n", 10, &w, nullptr);
            } else {
                OnStopped(r);
            }
        } else if ((r.type == '~' || r.type == '@') && !r.text.empty()) {
            PostMessageW(notify_, WM_DEBUG_ODS, 0, reinterpret_cast<LPARAM>(new std::string(r.text)));
        }
    }
    if (!exited) {
        d_.stopped_ = false;
        d_.running_ = false;
        PostMessageW(notify_, WM_DEBUG_EXITED, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(new std::string()));
    }
}

fs::path GdbSession::Resolve(const std::string& file, const std::string& fullname) {
    if (!fullname.empty()) {
        fs::path p = Mi::FromWsl(fullname);
        if (FileExists(p.wstring())) return NormalizePath(p);
    }
    std::wstring name = fs::path(Widen(file)).filename().wstring();
    auto it = sources_.find(Lower(name));
    if (it != sources_.end()) return it->second;
    return NormalizePath(dir_ / Widen(file));
}

void GdbSession::OnStopped(const Mi::Record& r) {
    ReadRegisters();
    std::string reason = r.results.Str("reason");
    DebugStop::Reason why = DebugStop::Reason::Step;
    std::wstring message;
    if (reason == "breakpoint-hit") {
        why = r.results.Str("disp") == "del" ? DebugStop::Reason::Entry : DebugStop::Reason::Breakpoint;
    } else if (reason == "location-reached") {
        why = DebugStop::Reason::RunTo;
    } else if (reason == "watchpoint-trigger") {
        why = DebugStop::Reason::Watchpoint;
        const Mi::Value* wpt = r.results.Get("wpt");
        const Mi::Value* value = r.results.Get("value");
        std::string exp = wpt ? wpt->Str("exp") : "";
        int number = wpt ? atoi(wpt->Str("number").c_str()) : 0;
        for (const auto& [n, w] : watches_) {
            if (n == number) exp = w.expression;
        }
        message = Widen(exp) + Tr(L" изменилось: ") + Widen(value ? value->Str("old") : "") + L" → " +
                  Widen(value ? value->Str("new") : "");
    } else if (reason == "signal-received") {
        std::string name = r.results.Str("signal-name");
        why = name == "SIGINT" || name == "SIGTRAP" ? DebugStop::Reason::Pause : DebugStop::Reason::Exception;
        if (why == DebugStop::Reason::Exception) message = Tr(L"Сигнал ") + Widen(name) + L" — " + Widen(r.results.Str("signal-meaning"));
    }
    SourceLocation loc;
    if (const Mi::Value* frame = r.results.Get("frame")) {
        int line = atoi(frame->Str("line").c_str());
        if (line > 0) loc = {Resolve(frame->Str("file"), frame->Str("fullname")), line};
    }
    d_.stopped_ = true;
    auto* stop = new DebugStop{why, message, loc, d_.context_};
    PostMessageW(notify_, WM_DEBUG_STOPPED, 0, reinterpret_cast<LPARAM>(stop));
}

void GdbSession::ReadRegisters() {
    if (registerNames_.empty()) {
        if (auto r = Command("-data-list-register-names")) {
            if (const Mi::Value* names = r->results.Get("register-names")) {
                for (const Mi::Value& v : names->items) registerNames_.push_back(v.text);
            }
        }
    }
    auto r = Command("-data-list-register-values x");
    if (!r) return;
    const Mi::Value* values = r->results.Get("register-values");
    if (!values) return;
    CONTEXT& c = d_.context_;
    ZeroMemory(&c, sizeof(c));
    c.ContextFlags = CONTEXT_ALL;
    DWORD64* regs[] = {&c.Rax, &c.Rbx, &c.Rcx, &c.Rdx, &c.Rsi, &c.Rdi, &c.Rbp, &c.Rsp,
                       &c.R8, &c.R9, &c.R10, &c.R11, &c.R12, &c.R13, &c.R14, &c.R15, &c.Rip};
    static const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8",
                                  "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"};
    for (const Mi::Value& v : values->items) {
        size_t number = static_cast<size_t>(atoi(v.Str("number").c_str()));
        if (number >= registerNames_.size()) continue;
        const std::string& name = registerNames_[number];
        std::string text = v.Str("value");
        for (int i = 0; i < 17; i++) {
            if (name == names[i]) *regs[i] = Mi::ParseNumber(text);
        }
        if (name == "eflags") c.EFlags = static_cast<DWORD>(Mi::ParseNumber(text));
        if (name.rfind("xmm", 0) == 0 && name.size() <= 5) {
            int x = atoi(name.c_str() + 3);
            size_t at = text.find("uint128 = 0x");
            if (x < 16 && at != std::string::npos) {
                std::string hex = text.substr(at + 12);
                hex = hex.substr(0, hex.find_first_not_of("0123456789abcdefABCDEF"));
                std::string low = hex.size() > 16 ? hex.substr(hex.size() - 16) : hex;
                std::string high = hex.size() > 16 ? hex.substr(0, hex.size() - 16) : "0";
                c.FltSave.XmmRegisters[x].Low = strtoull(low.c_str(), nullptr, 16);
                c.FltSave.XmmRegisters[x].High = static_cast<LONGLONG>(strtoull(high.c_str(), nullptr, 16));
            }
        }
    }
}

void GdbSession::InsertBreakpoints(const std::wstring& key, const std::map<int, std::string>& lines) {
    fs::path file = key;
    sources_[Lower(file.filename().wstring())] = file;
    auto& numbers = fileBreakpoints_[key];
    for (const auto& [line, raw] : lines) {
        std::string where = Mi::Quote(BaseName(file) + ":" + std::to_string(line));
        std::string cond = raw, message;
        bool logpoint = false;
        if (size_t log = cond.find("log:"); log != std::string::npos) {
            logpoint = true;
            message = cond.substr(log + 4);
            if (!message.empty() && message[0] == ' ') message.erase(0, 1);
            cond.erase(log);
        }
        std::string options;
        size_t first = cond.find_first_not_of(" \t");
        cond = first == std::string::npos ? "" : cond.substr(first);
        if (!cond.empty() && cond[0] == '#') {  // stop on the N-th hit: ignore N-1
            size_t end = 1;
            while (end < cond.size() && isdigit(static_cast<unsigned char>(cond[end]))) end++;
            int n = atoi(cond.substr(1, end - 1).c_str());
            if (n > 1) options += " -i " + std::to_string(n - 1);
            cond = cond.substr(end);
        }
        if (cond.find_first_not_of(" \t") != std::string::npos) options += " -c " + Mi::Quote(Mi::GdbExpression(cond));
        std::optional<Mi::Record> r;
        if (logpoint) {
            auto [format, args] = Mi::LogFormat(message + "\n");
            std::string command = "-dprintf-insert" + options + " " + where + " " + Mi::Quote(format);
            for (const std::string& a : args) command += " " + Mi::Quote(a);
            r = Command(command);
        } else {
            r = Command("-break-insert" + options + " " + where);
        }
        if (r && r->klass == "done") {
            if (const Mi::Value* bkpt = r->results.Get("bkpt")) numbers.push_back(atoi(bkpt->Str("number").c_str()));
        }
    }
}

void GdbSession::SetBreakpoints(const fs::path& file, const std::map<int, std::string>& lines) {
    std::wstring key = Debugger::Key(file);
    auto it = fileBreakpoints_.find(key);
    if (it != fileBreakpoints_.end() && !it->second.empty()) {
        std::string command = "-break-delete";
        for (int n : it->second) command += " " + std::to_string(n);
        Command(command);
        it->second.clear();
    }
    InsertBreakpoints(key, lines);
}

void GdbSession::Exec(const std::string& command) {
    d_.stopped_ = false;
    Command(command);
}

void GdbSession::Interrupt() {
    Command("-exec-interrupt", 2000);
}

void GdbSession::Stop() {
    if (!process_) return;
    if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        DWORD n;
        WriteFile(in_, "-gdb-exit\n", 10, &n, nullptr);
        if (WaitForSingleObject(process_, 2000) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
    }
}

bool GdbSession::RunTo(const SourceLocation& where) {
    sources_[Lower(where.file.filename().wstring())] = where.file;
    d_.stopped_ = false;
    auto r = Command("-exec-until " + Mi::Quote(BaseName(where.file) + ":" + std::to_string(where.line)));
    return r && r->klass != "error";
}

size_t GdbSession::ReadMemory(uint64_t address, void* buffer, size_t size) {
    auto r = Command("-data-read-memory-bytes " + Hex(address) + " " + std::to_string(size));
    if (!r || r->klass != "done") return 0;
    const Mi::Value* memory = r->results.Get("memory");
    if (!memory) return 0;
    auto* out = static_cast<uint8_t*>(buffer);
    size_t got = 0;
    for (const Mi::Value& block : memory->items) {
        uint64_t begin = Mi::ParseNumber(block.Str("begin"));
        std::string hex = block.Str("contents");
        if (begin != address + got) break;  // only the readable prefix
        for (size_t i = 0; i + 1 < hex.size() && got < size; i += 2) {
            out[got++] = static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        }
    }
    return got;
}

bool GdbSession::WriteMemory(uint64_t address, const void* data, size_t size) {
    std::string hex;
    char b[4];
    for (size_t i = 0; i < size; i++) {
        sprintf_s(b, "%02x", static_cast<const uint8_t*>(data)[i]);
        hex += b;
    }
    auto r = Command("-data-write-memory-bytes " + Hex(address) + " " + hex);
    return r && r->klass == "done";
}

bool GdbSession::SetRegister(const std::string& name, uint64_t value) {
    std::string target = name == "flags" || name == "rflags" ? "eflags" : name;
    auto r = Command("-data-evaluate-expression " + Mi::Quote("$" + target + "=" + std::to_string(static_cast<int64_t>(value))));
    if (!r || r->klass != "done") return false;
    ReadRegisters();
    return true;
}

std::optional<uint64_t> GdbSession::AddressOfLine(const SourceLocation& where) {
    std::string out = Console("info line " + BaseName(where.file) + ":" + std::to_string(where.line));
    size_t at = out.find("address ");
    if (at == std::string::npos) return std::nullopt;
    return Mi::ParseNumber(out.substr(at + 8));
}

bool GdbSession::SetNextStatement(const SourceLocation& where, std::wstring& error) {
    auto address = AddressOfLine(where);
    if (!address) {
        error = Tr(L"на этой строке нет кода");
        return false;
    }
    if (!SetRegister("pc", *address)) {
        error = Tr(L"gdb не изменил rip");
        return false;
    }
    return true;
}

std::optional<uint64_t> GdbSession::LookupSymbol(const std::string& name) {
    auto r = Command("-data-evaluate-expression " + Mi::Quote("&" + name));
    if (!r || r->klass != "done") return std::nullopt;
    std::string v = r->results.Str("value");
    if (v.find("0x") == std::string::npos) return std::nullopt;
    return Mi::ParseNumber(v.substr(v.find("0x")));
}

std::wstring GdbSession::SymbolAt(uint64_t address) {
    std::string out = Console("info symbol " + Hex(address));  // "main + 5 in section .text"
    size_t in = out.find(" in section");
    if (in == std::string::npos || out.rfind("No symbol", 0) == 0) return {};
    std::string s = out.substr(0, in);
    size_t plus = s.find(" + ");
    if (plus != std::string::npos) s = s.substr(0, plus) + "+" + Hex(Mi::ParseNumber(s.substr(plus + 3)));
    return Widen(s);
}

SourceLocation GdbSession::LocationOf(uint64_t address) {
    std::string out = Console("info line *" + Hex(address));  // Line 13 of "main.asm" starts at address ...
    if (out.rfind("Line ", 0) != 0) return {};
    int line = atoi(out.c_str() + 5);
    size_t q1 = out.find('"'), q2 = q1 == std::string::npos ? q1 : out.find('"', q1 + 1);
    if (line <= 0 || q2 == std::string::npos) return {};
    return {Resolve(out.substr(q1 + 1, q2 - q1 - 1), ""), line};
}

std::vector<Debugger::Frame> GdbSession::CallStack() {
    std::vector<Debugger::Frame> frames;
    auto r = Command("-stack-list-frames 0 63");
    if (!r || r->klass != "done") return frames;
    const Mi::Value* stack = r->results.Get("stack");
    if (!stack) return frames;
    for (const auto& [name, frame] : stack->fields) {
        Debugger::Frame f;
        f.pc = Mi::ParseNumber(frame.Str("addr"));
        int line = atoi(frame.Str("line").c_str());
        if (line > 0) f.location = {Resolve(frame.Str("file"), frame.Str("fullname")), line};
        f.function = Widen(frame.Str("func"));
        frames.push_back(f);
    }
    return frames;
}

bool GdbSession::AddDataWatch(const DataWatch& watch, std::wstring& error) {
    const char* type = watch.size == 1 ? "unsigned char" : watch.size == 2 ? "unsigned short" : watch.size == 4 ? "unsigned int" : "long";
    auto r = Command("-break-watch " + Mi::Quote(std::string("*(") + type + "*)" + Hex(watch.address)));
    if (!r || r->klass != "done") {
        error = L"gdb: " + Widen(r ? r->results.Str("msg") : Narrow(Tr(L"нет ответа")));
        return false;
    }
    const Mi::Value* wpt = r->results.Get("wpt");
    DataWatch w = watch;
    ReadMemory(w.address, &w.value, w.size);
    watches_.push_back({wpt ? atoi(wpt->Str("number").c_str()) : 0, w});
    return true;
}

void GdbSession::RemoveDataWatch(size_t index) {
    if (index >= watches_.size()) return;
    Command("-break-delete " + std::to_string(watches_[index].first));
    watches_.erase(watches_.begin() + index);
}

std::vector<DataWatch> GdbSession::DataWatches() {
    std::vector<DataWatch> out;
    for (auto& [n, w] : watches_) {
        ReadMemory(w.address, &w.value, w.size);
        out.push_back(w);
    }
    return out;
}
