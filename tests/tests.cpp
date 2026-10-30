// Unit tests for the parts of AsmEditor that do not need the UI, plus an end-to-end debugger test that builds a
// small program with NASM and the linker from the settings and debugs it. Build and run: build.bat test
#include "../src/asmtools.h"
#include "../src/completion.h"
#include "../src/debugger.h"
#include "../src/hints.h"
#include "../src/project.h"
#include "../src/terminal.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

namespace {

int g_failed = 0, g_passed = 0;
std::string g_test;

#define CHECK(cond) Check((cond), #cond, __LINE__)
#define CHECK_EQ(a, b) CheckEq((a), (b), #a " == " #b, __LINE__)

void Check(bool ok, const char* what, int line) {
    if (ok) {
        g_passed++;
        return;
    }
    g_failed++;
    printf("FAIL [%s] line %d: %s\n", g_test.c_str(), line, what);
}

template <typename A, typename B>
void CheckEq(const A& a, const B& b, const char* what, int line) {
    bool ok = a == b;
    Check(ok, what, line);
    if (!ok) {
        if constexpr (std::is_convertible_v<A, std::string> && std::is_convertible_v<B, std::string>) {
            printf("    got:      [%s]\n    expected: [%s]\n", std::string(a).c_str(), std::string(b).c_str());
        } else if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) {
            printf("    got: %lld, expected: %lld\n", static_cast<long long>(a), static_cast<long long>(b));
        }
    }
}

struct Test {
    const char* name;
    void (*fn)();
};
std::vector<Test>& Tests() {
    static std::vector<Test> t;
    return t;
}
struct Register {
    Register(const char* name, void (*fn)()) { Tests().push_back({name, fn}); }
};
#define TEST(name)                          \
    void name();                            \
    Register reg_##name(#name, name);       \
    void name()

std::vector<std::pair<std::string, TokenKind>> Tokens(const std::string& line) {
    std::vector<Token> tokens;
    Tokenize(line.data(), static_cast<int>(line.size()), tokens);
    std::vector<std::pair<std::string, TokenKind>> out;
    for (const Token& t : tokens) out.push_back({line.substr(t.start, t.end - t.start), t.type});
    return out;
}

// ---- tokenizer ------------------------------------------------------------------------------------------------

TEST(TokenizerLabelInstructionComment) {
    auto t = Tokens("main:   mov rax, [msg + 8] ; comment");
    CHECK_EQ(t.size(), 5u);
    CHECK(t[0].second == T_LABEL);
    CHECK(t[1] == std::make_pair(std::string("mov"), T_INSTRUCTION));
    CHECK(t[2] == std::make_pair(std::string("rax"), T_REGISTER));
    CHECK(t[3] == std::make_pair(std::string("8"), T_NUMBER));
    CHECK(t[4].second == T_COMMENT);
}

TEST(TokenizerDataLabelStringAndPreprocessor) {
    auto t = Tokens("msg db \"a;b\", 10, 0");
    CHECK(t[0] == std::make_pair(std::string("msg"), T_LABEL));
    CHECK(t[1] == std::make_pair(std::string("db"), T_DIRECTIVE));
    CHECK(t[2] == std::make_pair(std::string("\"a;b\""), T_STRING));  // ';' inside a string is not a comment
    auto p = Tokens("%define SIZE 10");
    CHECK(p[0] == std::make_pair(std::string("%define"), T_PREPROCESSOR));
}

// ---- NASM errors -----------------------------------------------------------------------------------------------

TEST(ParseNasmErrorsAndWarnings) {
    auto d = Toolchain::ParseNasm("C:\\p\\main.asm:12: error: symbol `foo' not defined\n"
                                  "C:\\p\\inc.inc:3: warning: label alone on a line\n"
                                  "nasm: fatal: unable to open input file\n");
    CHECK_EQ(d.size(), 2u);
    CHECK_EQ(d[0].line, 12);
    CHECK(d[0].severity == Severity::Error);
    CHECK_EQ(Narrow(d[0].message), std::string("symbol `foo' not defined"));
    CHECK(d[1].severity == Severity::Warning);
    CHECK_EQ(Narrow(d[1].file.filename().wstring()), std::string("inc.inc"));
}

// ---- symbols, occurrences, rename checks -----------------------------------------------------------------------

const char* kSource =
    "global main\n"
    "main:\n"
    "    mov ecx, 3\n"
    ".loop:\n"
    "    dec ecx\n"
    "    jnz .loop          ; .loop in a comment\n"
    "    jmp main.loop\n"
    "other:\n"
    ".loop:\n"
    "    jmp .loop\n";

TEST(ScanSymbolsScopes) {
    auto s = Completion::ScanSymbols(kSource);
    int locals = 0;
    for (const auto& sym : s) {
        if (sym.name == ".loop") {
            locals++;
            CHECK(sym.scope == "main" || sym.scope == "other");
        }
    }
    CHECK_EQ(locals, 2);
}

TEST(FindOccurrencesLocalLabel) {
    auto o = Completion::FindOccurrences(kSource, ".loop", "main");
    // definition, jnz .loop, main.loop (qualified); not the comment, not other's .loop
    CHECK_EQ(o.size(), 3u);
    CHECK(o[0].definition);
    CHECK_EQ(o[0].line, 4);
    CHECK(o[2].qualified);
    auto g = Completion::FindOccurrences(kSource, "main", "", true);
    CHECK_EQ(g.size(), 3u);  // global main, main:, main. prefix of main.loop
}

TEST(NamesValidAndReserved) {
    CHECK(Completion::IsValidName("my_label"));
    CHECK(Completion::IsValidName(".loop"));
    CHECK(!Completion::IsValidName("1abc"));
    CHECK(!Completion::IsValidName("a b"));
    CHECK(Completion::IsReservedWord("RAX"));
    CHECK(Completion::IsReservedWord("mov"));
    CHECK(!Completion::IsReservedWord("counter"));
}

TEST(CompletionContext) {
    auto list = Completion::BuildList("    mov ", "ra", {});
    CHECK(list.find("rax?") != std::string::npos);
    CHECK(list.find("mov?") == std::string::npos);  // operands: registers, not instructions
    CHECK(Completion::BuildList("    mov rax, 1 ; ", "ra", {}).empty());  // inside a comment
    auto start = Completion::BuildList("    ", "mo", {});
    CHECK(start.find("mov?") != std::string::npos);
}

// ---- terminal emulator -----------------------------------------------------------------------------------------

std::vector<std::string> ScreenText(Terminal& t) {
    size_t cursor = 0;
    std::vector<std::string> out;
    for (const TermLine& l : t.Screen(cursor)) out.push_back(l.text);
    while (!out.empty() && out.back().find_first_not_of(' ') == std::string::npos) out.pop_back();
    for (auto& s : out) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
    }
    return out;
}

TEST(TerminalTextAndCursorMoves) {
    Terminal t(40, 5);
    std::string data = "hello\r\nworld\x1b[1;1HJ";
    t.Feed(data.data(), data.size());
    auto s = ScreenText(t);
    CHECK_EQ(s.size(), 2u);
    CHECK_EQ(s[0], std::string("Jello"));
    CHECK_EQ(s[1], std::string("world"));
}

TEST(TerminalScrollsIntoHistory) {
    Terminal t(20, 3);
    std::string data;
    for (int i = 0; i < 6; i++) data += "line" + std::to_string(i) + "\r\n";
    t.Feed(data.data(), data.size());
    auto history = t.TakeHistory();
    CHECK(history.size() >= 3u);
    CHECK_EQ(history[0].text.substr(0, 5), std::string("line0"));
}

TEST(TerminalColors) {
    Terminal t(20, 2);
    std::string data = "\x1b[31mred\x1b[0m";
    t.Feed(data.data(), data.size());
    size_t cursor = 0;
    auto lines = t.Screen(cursor);
    CHECK(!lines.empty() && !lines[0].runs.empty());
    CHECK_EQ(lines[0].runs[0].second, Terminal::kStyleBase + 1);
}

// ---- formatter -------------------------------------------------------------------------------------------------

TEST(FormatterColumns) {
    AsmFormat::Options o{4, 12, 32};
    CHECK_EQ(AsmFormat::FormatLine("mov rax,5;x", o), std::string("    mov     rax, 5              ;x"));
    CHECK_EQ(AsmFormat::FormatLine("main:", o), std::string("main:"));
    CHECK_EQ(AsmFormat::FormatLine("  main :  ret", o), std::string("main: ret")  /* label wider than the indent: one space */);
    CHECK_EQ(AsmFormat::FormatLine("msg db \"a,b\",10,0", o), std::string("    msg     db \"a,b\", 10, 0"));
    CHECK_EQ(AsmFormat::FormatLine("   section .text", o), std::string("section .text"));
    CHECK_EQ(AsmFormat::FormatLine("  ; just a comment", o), std::string("  ; just a comment"));
    CHECK_EQ(AsmFormat::FormatLine("rep movsb", o), std::string("    rep movsb"));
    CHECK_EQ(AsmFormat::FormatLine("%define X 1", o), std::string("%define X 1"));
    CHECK_EQ(AsmFormat::FormatLine("lea rcx,[ rbx + 8 ]", o), std::string("    lea     rcx, [rbx + 8]"));
}

TEST(FormatterKeepsLineEndings) {
    AsmFormat::Options o;
    std::string text = "main:\r\nret\r\n";
    CHECK_EQ(AsmFormat::Format(text, o, "\r\n"), std::string("main:\r\n    ret\r\n"));
}

// ---- calling convention check ----------------------------------------------------------------------------------

TEST(StackCheckAligned) {
    auto d = StackCheck::Analyze("extern printf\nmain:\n    sub rsp, 40\n    call printf\n    add rsp, 40\n    ret\n");
    CHECK(d.empty());
    auto pushed = StackCheck::Analyze("extern printf\nmain:\n    push rbx\n    sub rsp, 32\n    call printf\n");
    CHECK(pushed.empty());
}

TEST(StackCheckMisalignedAndNoShadow) {
    auto d = StackCheck::Analyze("extern printf\nmain:\n    sub rsp, 32\n    call printf\n");
    CHECK_EQ(d.size(), 1u);
    if (!d.empty()) CHECK_EQ(d[0].line, 4);
    auto shadow = StackCheck::Analyze("extern puts\nmain:\n    sub rsp, 8\n    call puts\n");
    CHECK_EQ(shadow.size(), 1u);
    // own functions and unknown rsp changes are not reported
    CHECK(StackCheck::Analyze("main:\n    call helper\nhelper:\n    ret\n").empty());
    CHECK(StackCheck::Analyze("extern printf\nmain:\n    mov rsp, rax\n    call printf\n").empty());
}

TEST(TemplatesPassStackCheck) {
    for (const auto& t : Templates::All()) CHECK(StackCheck::Analyze(t.text).empty());
    for (const auto& e : Exercises::All()) CHECK(StackCheck::Analyze(e.starter).empty());
}

// ---- disassembler output, exercises ----------------------------------------------------------------------------

TEST(DisasmParse) {
    auto lines = Disasm::Parse("00401000  4883EC28          sub rsp,byte +0x28\n"
                               "00401004  48B8EFCDAB896745  mov rax,0x123456789abcdef\n"
                               "         -2301\n");
    CHECK_EQ(lines.size(), 2u);
    CHECK_EQ(lines[0].address, 0x401000ull);
    CHECK_EQ(lines[0].text, std::string("sub rsp,byte +0x28"));
    CHECK_EQ(lines[1].bytes, std::string("48B8EFCDAB8967452301"));
}

TEST(ExercisesMatching) {
    CHECK(Exercises::OutputMatches("Сумма: 5\r\n", "5"));
    CHECK(!Exercises::OutputMatches("55\n", "5"));
    CHECK(Exercises::OutputMatches("a  b\n c", "b c"));
    CHECK_EQ(Exercises::IdFromSource("; task: sum\r\n; ..."), std::string("sum"));
    CHECK(Exercises::Find("factorial") != nullptr);
    for (const auto& e : Exercises::All()) CHECK_EQ(Exercises::IdFromSource(e.starter), e.id);
}

TEST(SnippetCaret) {
    size_t caret = 0;
    std::string text = Snippets::Expand(Snippets::All()[1], "\r\n", caret);
    CHECK(text.find('|') == std::string::npos);
    CHECK(caret > 0 && caret < text.size());
}

// ---- reference -------------------------------------------------------------------------------------------------

TEST(ReferenceCoversEveryInstruction) {
    std::vector<std::string> missing;
    for (const std::string& w : SyntaxWords(WordClass::Instruction)) {
        if (Hints::For(w).empty()) missing.push_back(w);
    }
    for (const std::string& w : SyntaxWords(WordClass::Prefix)) {
        if (w[0] != 'o' && w[0] != 'a' && w != "bnd" && w != "xacquire" && w != "xrelease" && Hints::For(w).empty()) {
            missing.push_back(w);
        }
    }
    std::sort(missing.begin(), missing.end());
    if (!missing.empty()) {
        printf("    no card for %zu words:", missing.size());
        for (size_t i = 0; i < missing.size() && i < 60; i++) printf(" %s", missing[i].c_str());
        printf("\n");
    }
    CHECK(missing.empty());
    CHECK(!Hints::Function("printf").empty());
    CHECK(Hints::For("vaddps").find("AVX") != std::string::npos);
}

// ---- expressions -----------------------------------------------------------------------------------------------

TEST(ExpressionsWithoutProcess) {
    Debugger d(nullptr);
    std::wstring err;
    auto eval = [&](const char* e) { return d.Evaluate(e, err); };
    CHECK_EQ(*eval("2 + 3 * 4"), 14ull);
    CHECK_EQ(*eval("(1 << 4) | 1"), 17ull);
    CHECK_EQ(*eval("0x10 - 2h"), 14ull);
    CHECK_EQ(*eval("5 == 5 && 3 < 4"), 1ull);
    CHECK_EQ(*eval("-1 < 0"), 1ull);  // comparisons are signed
    CHECK_EQ(*eval("rax + 7"), 7ull);  // registers are 0 without a program
    CHECK_EQ(*eval("17 % 5"), 2ull);
    CHECK(!eval("1 / 0"));
    CHECK(!eval("nosuchname"));
    CONTEXT c{};
    CHECK(Debugger::SetRegisterIn(c, "eax", 0x1234567890ull));
    CHECK_EQ(c.Rax, 0x34567890ull);  // 32-bit writes clear the upper half
    c.Rax = 0xFFFF;
    CHECK(Debugger::SetRegisterIn(c, "al", 1));
    CHECK_EQ(c.Rax, 0xFF01ull);
}

// ---- localization, project sources ------------------------------------------------------------------------

TEST(Localization) {
    SetLanguage(L"en");
    CHECK_EQ(Narrow(Tr(L"Собрать")), std::string("Build"));
    CHECK_EQ(Tr(std::string("Проблем нет.\n")), std::string("No problems.\n"));
    CHECK_EQ(Tr(std::string("Отладка")) + "!", std::string("Debug!"));
    CHECK_EQ(Narrow(Tr(L"нет такого текста")), std::string("нет такого текста"));  // unknown text stays
    SetLanguage(L"ru");
    CHECK_EQ(Narrow(Tr(L"Собрать")), std::string("Собрать"));
}

TEST(ProjectExcludedAndIncludedFiles) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path dir = fs::path(tmp) / L"asmeditor-tests" / L"collect";
    fs::remove_all(dir);
    fs::create_directories(dir / L"sub");
    std::ofstream(dir / L"main.asm") << "%include \"defs.inc\"\nglobal main\nmain:\n ret\n";
    std::ofstream(dir / L"defs.inc") << "X equ 1\n";
    std::ofstream(dir / L"sub" / L"util.asm") << "global helper\nhelper:\n ret\n";
    std::ofstream(dir / L"skip.asm") << "nop\n";
    std::ofstream(dir / L"other.asm") << "global main\nmain:\n ret\n";  // another program
    auto all = Project::Collect(dir, dir / L"main.asm", {});
    CHECK_EQ(all.files.size(), 3u);  // main, skip, sub/util (not the .inc, not other.asm)
    CHECK_EQ(all.otherPrograms.size(), 1u);
    auto excluded = Project::Collect(dir, dir / L"main.asm", {Project::Key(dir / L"skip.asm")});
    CHECK_EQ(excluded.files.size(), 2u);
    CHECK(Project::DefinesMain(dir / L"other.asm"));
    CHECK_EQ(Project::Dependencies(dir / L"main.asm", {dir}).size(), 1u);
}

// ---- debugger end to end ---------------------------------------------------------------------------------------

struct DebugHost {
    HWND hwnd = nullptr;
    HPCON pty = nullptr;
    HANDLE in = nullptr, out = nullptr;
    std::thread drain;
    std::vector<std::unique_ptr<DebugStop>> stops;
    bool exited = false;
    DWORD exitCode = 0;
    std::string stats;

    DebugHost() {
        hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        HANDLE inRead, outWrite;
        CreatePipe(&inRead, &in, nullptr, 0);
        CreatePipe(&out, &outWrite, nullptr, 0);
        CreatePseudoConsole(COORD{80, 25}, inRead, outWrite, 0, &pty);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        HANDLE o = out;
        drain = std::thread([o] {
            char buf[4096];
            DWORD n;
            while (ReadFile(o, buf, sizeof(buf), &n, nullptr) && n) {
            }
        });
    }
    ~DebugHost() {
        ClosePseudoConsole(pty);
        CloseHandle(in);
        if (drain.joinable()) drain.join();
        CloseHandle(out);
        DestroyWindow(hwnd);
    }
    // Pumps the debugger's messages until a stop or the exit (false on timeout).
    bool Wait(DWORD timeoutMs) {
        size_t before = stops.size();
        auto start = GetTickCount64();
        while (GetTickCount64() - start < timeoutMs) {
            MSG msg;
            while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_DEBUG_STOPPED) stops.emplace_back(reinterpret_cast<DebugStop*>(msg.lParam));
                else if (msg.message == WM_DEBUG_EXITED) {
                    exited = true;
                    exitCode = static_cast<DWORD>(msg.wParam);
                    std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(msg.lParam));
                    if (s) stats = *s;
                } else if (msg.message == WM_DEBUG_LOG || msg.message == WM_DEBUG_ODS) {
                    delete reinterpret_cast<std::string*>(msg.lParam);
                }
            }
            if (stops.size() > before || exited) return true;
            Sleep(5);
        }
        return false;
    }
};

std::optional<fs::path> BuildProgram(const std::string& name, const std::string& source) {
    Settings s;
    s.Load();
    Toolchain::ApplyDefaults(s);
    if (!Toolchain::NasmAvailable(s) || !Toolchain::LinkerAvailable(s) || s.linkerPreset != L"MSVC_CRT") {
        printf("    (skipped: needs NASM and MSVC link with the C runtime preset)\n");
        return std::nullopt;
    }
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path dir = fs::path(tmp) / L"asmeditor-tests" / Widen(name);
    fs::create_directories(dir);
    fs::path src = dir / L"main.asm";
    std::ofstream(src, std::ios::binary) << source;
    BuildPlan plan;
    plan.sources = {src};
    plan.root = dir;
    plan.outDir = dir / L"build";
    plan.exe = plan.outDir / L"main.exe";
    plan.rebuild = true;
    std::string log;
    BuildResult r = Toolchain::Build(s, plan, [&](const std::string& t, Out) { log += t; });
    if (!r.ok) {
        printf("    build failed:\n%s\n", log.c_str());
        return std::nullopt;
    }
    return r.exe;
}

const char* kCounterProgram =
    "default rel\n"                // 1
    "extern printf\n"              // 2
    "global main\n"                // 3
    "section .data\n"              // 4
    "    counter dq 0\n"           // 5
    "    fmt db \"%lld\", 10, 0\n" // 6
    "section .text\n"              // 7
    "main:\n"                      // 8
    "    sub rsp, 40\n"            // 9
    "    xor ebx, ebx\n"           // 10
    ".loop:\n"                     // 11
    "    inc qword [counter]\n"    // 12
    "    inc rbx\n"                // 13
    "    call helper\n"            // 14
    "    cmp rbx, 5\n"             // 15
    "    jl .loop\n"               // 16
    "    lea rcx, [fmt]\n"         // 17
    "    mov rdx, [counter]\n"     // 18
    "    call printf\n"            // 19
    "    add rsp, 40\n"            // 20
    "    xor eax, eax\n"           // 21
    "    ret\n"                    // 22
    "helper:\n"                    // 23
    "    nop\n"                    // 24
    "    ret\n";                   // 25

TEST(DebuggerConditionsWatchpointsCallStack) {
    auto exe = BuildProgram("counter", kCounterProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    Debugger::Breakpoints bps{{Debugger::Key(src), {{15, "rbx == 3"}, {24, "#2"}}}};
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, bps, false, std::nullopt, error));
    // helper (line 24) on its 2nd call: rbx == 2; the call stack goes back to main.
    CHECK(host.Wait(10000));
    CHECK(!host.stops.empty());
    if (host.stops.empty()) return;
    CHECK_EQ(host.stops.back()->location.line, 24);
    CHECK_EQ(d.Context().Rbx, 2ull);
    auto frames = d.CallStack();
    CHECK(frames.size() >= 2u);
    if (frames.size() >= 2) {
        CHECK_EQ(frames[1].location.line, 14);
        CHECK(Narrow(frames[1].function).find("main") != std::string::npos);
    }
    // line 15 with rbx == 3
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK_EQ(host.stops.back()->location.line, 15);
    CHECK_EQ(d.Context().Rbx, 3ull);
    // data breakpoint on counter: stops after the next write (counter 3 -> 4), right after line 12
    std::wstring err;
    auto counter = d.LookupSymbol("counter");
    CHECK(counter.has_value());
    if (counter) {
        DataWatch w;
        w.expression = "counter";
        w.address = *counter;
        w.size = 8;
        CHECK(d.AddDataWatch(w, err));
        d.Continue();
        CHECK(host.Wait(10000));
        CHECK(host.stops.back()->reason == DebugStop::Reason::Watchpoint);
        uint64_t value = 0;
        d.ReadMemory(*counter, &value, 8);
        CHECK_EQ(value, 4ull);
        CHECK_EQ(host.stops.back()->location.line, 13);
        d.RemoveDataWatch(0);
    }
    // edit a register: rbx = 4, so the loop runs once more and the program prints 5
    CHECK(d.SetRegister("rbx", 4));
    d.SetBreakpoints(src, {});
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK(host.exited);
    CHECK_EQ(host.exitCode, 0ul);
    CHECK(host.stats.find("пик памяти") != std::string::npos || host.stats.find("peak") != std::string::npos);
}

const char* kSleepProgram =
    "default rel\n"
    "extern Sleep\n"
    "global main\n"
    "section .text\n"
    "main:\n"
    "    sub rsp, 40\n"
    ".loop:\n"
    "    mov ecx, 3000\n"
    "    call Sleep\n"   // 9
    "    jmp .loop\n";   // 10

// Pause while the program waits in a system call, then step: the step must not take seconds.
TEST(DebuggerStepAfterPauseIsFast) {
    auto exe = BuildProgram("sleep", kSleepProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, {}, false, std::nullopt, error));
    Sleep(700);
    d.Pause();
    CHECK(host.Wait(5000));
    if (host.stops.empty()) return;
    CHECK(host.stops.back()->reason == DebugStop::Reason::Pause);
    auto start = std::chrono::steady_clock::now();
    d.StepOver();
    CHECK(host.Wait(8000));  // the program sleeps up to 3 s before it returns to a source line
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    printf("    step after pause: %lld ms (includes the remaining Sleep)\n", static_cast<long long>(ms));
    CHECK(host.stops.size() >= 2u);
    if (host.stops.size() >= 2) {
        CHECK(host.stops.back()->location.valid());
        // A second step is an ordinary one on the main thread: fast.
        start = std::chrono::steady_clock::now();
        d.StepOver();
        CHECK(host.Wait(5000));
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        printf("    next step: %lld ms\n", static_cast<long long>(ms));
        CHECK(ms < 1500 || host.stops.back()->location.line == 9);
    }
    d.Stop();
    host.Wait(5000);
}
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    for (const Test& t : Tests()) {
        g_test = t.name;
        int before = g_failed;
        printf("%s\n", t.name);
        t.fn();
        if (g_failed != before) printf("  -> FAILED\n");
    }
    printf("\n%d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
