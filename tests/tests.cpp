// Unit tests for the parts of AsmEditor that do not need the UI, plus an end-to-end debugger test that builds a
// small program with NASM and the linker from the settings and debugs it. Build and run: build.bat test
#include "../src/asmtools.h"
#include "../src/completion.h"
#include "../src/debugger.h"
#include "../src/gdbmi.h"
#include "../src/hints.h"
#include "../src/project.h"
#include "../src/terminal.h"
#include "../src/update.h"
#include "../src/version.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
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

TEST(OccurrencesOfMacrosAndDefines) {
    std::string text =
        "%define SIZE 10\n"
        "%assign COUNT SIZE * 2\n"
        "%macro print 1\n"
        "    lea rcx, [%1]\n"
        "    mov edx, SIZE\n"
        "%%again:\n"
        "    jmp %%again\n"
        "%endmacro\n"
        "main:\n"
        "    print msg\n"
        "    mov rax, %[SIZE]\n"
        "%ifdef SIZE\n"
        "%undef SIZE\n"
        "%endif\n";
    auto size = Completion::FindOccurrences(text, "SIZE", "", false);
    CHECK_EQ(size.size(), 6u);
    int defs = 0;
    for (const auto& o : size) defs += o.definition;
    CHECK_EQ(defs, 1);
    auto print = Completion::FindOccurrences(text, "print", "", false);
    CHECK_EQ(print.size(), 2u);
    auto again = Completion::FindOccurrences(text, "%%again", "", false);
    CHECK_EQ(again.size(), 2u);
    auto count = Completion::FindOccurrences(text, "COUNT", "", false);
    CHECK(count.size() == 1u && count[0].definition);
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
    for (const auto& t : Templates::All()) {
        CHECK(StackCheck::Analyze(t.text).empty());
        for (const Diagnostic& d : Analyzer::Analyze(t.text)) {
            printf("    %s:%d %s\n", Narrow(t.fileName).c_str(), d.line, d.code.c_str());
            CHECK(false);
        }
    }
    for (const auto& e : Exercises::All()) CHECK(StackCheck::Analyze(e.starter).empty());
}

TEST(AnalyzerCalleeSavedDivUnreachable) {
    std::string text =
        "global f\n"            // 1
        "f:\n"                  // 2
        "    mov rbx, 1\n"      // 3  rbx not saved
        "    ret\n"             // 4
        "    mov eax, 2\n"      // 5  unreachable
        "global g\n"            // 6
        "g:\n"                  // 7
        "    push rbx\n"        // 8
        "    mov rbx, 2\n"      // 9
        "    mov rax, 10\n"     // 10
        "    mov rcx, 3\n"      // 11
        "    div rcx\n"         // 12 rdx not cleared
        "    xor edx, edx\n"    // 13
        "    div rcx\n"         // 14 fine
        "    div cl\n"          // 15 8-bit: fine
        "    jmp .done\n"       // 16
        ".done:\n"              // 17 reachable through the label
        "    pop rbx\n"         // 18
        "    ret\n";            // 19
    auto d = Analyzer::Analyze(text);
    CHECK_EQ(d.size(), 3u);
    if (d.size() == 3) {
        CHECK_EQ(d[0].line, 3);
        CHECK_EQ(d[0].code, std::string("callee-saved:rbx"));
        CHECK_EQ(d[1].line, 5);
        CHECK_EQ(d[1].code, std::string("unreachable"));
        CHECK_EQ(d[2].line, 12);
        CHECK_EQ(d[2].code, std::string("div-rdx"));
        // quick fixes remove the warnings
        auto fixes = Analyzer::QuickFixes(text, d[2]);
        CHECK_EQ(fixes.size(), 1u);
        std::string fixed = Analyzer::Apply(text, fixes[0].edits, "\n");
        CHECK(fixed.find("    xor edx, edx\n    div rcx\n    xor edx, edx") != std::string::npos);
        fixes = Analyzer::QuickFixes(fixed, d[0]);
        CHECK_EQ(fixes.size(), 1u);
        fixed = Analyzer::Apply(fixed, fixes[0].edits, "\n");
        CHECK(fixed.find("f:\n    push rbx\n    mov rbx, 1\n    pop rbx\n    ret\n") != std::string::npos);
        int calleeSaved = 0;
        for (const Diagnostic& x : Analyzer::Analyze(fixed)) calleeSaved += x.code.rfind("callee", 0) == 0 || x.code == "div-rdx";
        CHECK_EQ(calleeSaved, 0);
    }
}

TEST(AnalyzerClobberedAndOwnArguments) {
    std::string text =
        "extern printf\n"                 // 0
        "global main\n"                   // 1
        "main:\n"                         // 2
        "    sub rsp, 40\n"               // 3
        "    mov r8, 7\n"                 // 4
        "    lea rcx, [fmt]\n"            // 5
        "    call printf\n"               // 6
        "    add rax, r8\n"               // 7 r8 destroyed by printf
        "    mov rcx, 1\n"                // 8 written again: fine
        "    add rax, rcx\n"              // 9
        "    mov rdx, [rdx]\n"            // 10 rdx read (memory operand)
        "    add rsp, 40\n"
        "    ret\n"
        "; rcx = a, rdx = b\n"            // 13
        "; returns a + b in rax\n"        // 14
        "sum2:\n"                         // 15
        "    lea rax, [rcx + rdx]\n"
        "    ret\n"
        "f:\n"                            // 18
        "    mov ecx, 2\n"                // 19
        "    call sum2\n";                // 20
    int clobbered = 0;
    for (const Diagnostic& d : Analyzer::Analyze(text)) {
        if (d.code.rfind("clobbered:", 0) == 0) {
            clobbered++;
            CHECK(d.line == 8 || d.line == 11);
        }
    }
    CHECK_EQ(clobbered, 2);
    auto a = Analyzer::ArgumentsOfCall(text, 20);
    CHECK(a && a->known && a->arguments.size() == 2);
    if (a && a->arguments.size() == 2) {
        CHECK(a->arguments[0].name == "a" && a->arguments[0].setOnLine == 19);
        CHECK(a->arguments[1].name == "b" && a->arguments[1].setOnLine == -1);
    }
}

TEST(QuickFixesExternAndStack) {
    std::string text =
        "default rel\n"          // 1
        "extern puts\n"          // 2
        "global main\n"          // 3
        "section .text\n"        // 4
        "main:\n"                // 5
        "    sub rsp, 32\n"      // 6
        "    call printf\n"      // 7
        "    add rsp, 32\n"      // 8
        "    ret\n";             // 9
    Diagnostic undefined{{}, 7, Severity::Error, L"symbol `printf' not defined", "undefined:printf"};
    auto fixes = Analyzer::QuickFixes(text, undefined);
    CHECK_EQ(fixes.size(), 1u);
    if (!fixes.empty()) {
        std::string fixed = Analyzer::Apply(text, fixes[0].edits, "\n");
        CHECK(fixed.find("extern puts\nextern printf\nglobal main") != std::string::npos);
        auto stack = StackCheck::Analyze(fixed);
        CHECK_EQ(stack.size(), 1u);  // misaligned: 32 instead of 40
        if (stack.size() == 1) {
            CHECK_EQ(stack[0].code, std::string("stack-align"));
            auto align = Analyzer::QuickFixes(fixed, stack[0]);
            CHECK_EQ(align.size(), 1u);
            if (!align.empty()) {
                std::string done = Analyzer::Apply(fixed, align[0].edits, "\n");
                CHECK(done.find("sub rsp, 40") != std::string::npos && done.find("add rsp, 40") != std::string::npos);
                CHECK(StackCheck::Analyze(done).empty());
            }
        }
    }
    // CRLF text keeps its line endings
    std::string crlf = "a\r\nb\r\n";
    CHECK_EQ(Analyzer::Apply(crlf, {{1, false, "x"}}, "\r\n"), std::string("a\r\nx\r\nb\r\n"));
}

TEST(CallArguments) {
    std::string text =
        "extern printf\n"            // 0 (0-based)
        "extern WriteFile\n"         // 1
        "main:\n"                    // 2
        "    sub rsp, 56\n"          // 3
        "    lea rcx, [fmt]\n"       // 4
        "    mov rdx, 5\n"           // 5
        "    call printf\n"          // 6
        "    mov rcx, [handle]\n"    // 7
        "    lea rdx, [buf]\n"       // 8
        "    mov qword [rsp+32], 0\n" // 9
        "    call WriteFile\n"       // 10
        "    call helper\n";         // 11
    auto a = Analyzer::ArgumentsOfCall(text, 6);
    CHECK(a && a->known && a->variadic);
    if (a) {
        CHECK_EQ(a->arguments.size(), 1u);
        CHECK(a->arguments[0].where == "rcx" && a->arguments[0].name == "format" && a->arguments[0].setOnLine == 4);
        CHECK_EQ(a->extra.size(), 1u);
        if (!a->extra.empty()) CHECK(a->extra[0].where == "rdx" && a->extra[0].setOnLine == 5);
    }
    a = Analyzer::ArgumentsOfCall(text, 10);
    CHECK(a && a->known && !a->variadic);
    if (a && a->arguments.size() == 5) {
        CHECK_EQ(a->arguments[0].setOnLine, 7);
        CHECK_EQ(a->arguments[1].setOnLine, 8);
        CHECK_EQ(a->arguments[2].setOnLine, -1);  // r8 forgotten
        CHECK_EQ(a->arguments[3].setOnLine, -1);
        CHECK(a->arguments[4].where == "[rsp+32]" && a->arguments[4].setOnLine == 9);
    } else {
        CHECK(false);
    }
    a = Analyzer::ArgumentsOfCall(text, 11);  // unknown function, nothing set since the previous call
    CHECK(a && !a->known && a->arguments.empty());
    CHECK(!Analyzer::ArgumentsOfCall(text, 5));
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

TEST(ConditionsExplainJumps) {
    const uint32_t SF = 1 << 7, ZF = 1 << 6, OF = 1 << 11, CF = 1;
    auto all = Conditions::All(SF);
    CHECK_EQ(all.size(), 16u);
    CHECK(all[12].name == "l" && all[12].holds);    // SF != OF
    CHECK(all[13].name == "ge" && !all[13].holds);
    uint8_t jl[] = {0x7C, 0x05};
    auto j = Conditions::Decode(jl, 2, SF, 0, false);
    CHECK(j && j->mnemonic == "jl" && j->condition.holds && j->condition.values == "SF=1 OF=0");
    j = Conditions::Decode(jl, 2, SF | OF, 0, false);
    CHECK(j && !j->condition.holds);
    uint8_t jle[] = {0x0F, 0x8E, 0, 0, 0, 0};
    j = Conditions::Decode(jle, 6, ZF, 0, false);
    CHECK(j && j->mnemonic == "jle" && j->condition.holds);
    uint8_t cmovge[] = {0x48, 0x0F, 0x4D, 0xC1};
    j = Conditions::Decode(cmovge, 4, SF | OF, 0, false);
    CHECK(j && j->mnemonic == "cmovge" && j->kind == Conditions::Instruction::Kind::Move && j->condition.holds);
    uint8_t setb[] = {0x0F, 0x92, 0xC0};
    j = Conditions::Decode(setb, 3, CF, 0, false);
    CHECK(j && j->mnemonic == "setb" && j->condition.holds);
    uint8_t loop[] = {0xE2, 0xFE};
    j = Conditions::Decode(loop, 2, 0, 1, false);
    CHECK(j && j->mnemonic == "loop" && !j->condition.holds);  // rcx becomes 0
    j = Conditions::Decode(loop, 2, 0, 5, false);
    CHECK(j && j->condition.holds);
    uint8_t jrcxz[] = {0xE3, 0x02};
    j = Conditions::Decode(jrcxz, 2, 0, 0, false);
    CHECK(j && j->mnemonic == "jrcxz" && j->condition.holds);
    j = Conditions::Decode(jrcxz, 2, 0, 0, true);
    CHECK(j && j->mnemonic == "jecxz");
    uint8_t mov[] = {0x48, 0x89, 0xC8};
    CHECK(!Conditions::Decode(mov, 3, 0, 0, false));
}

// In English every reference card (instructions, AVX forms, registers, directives, functions) is in English.
TEST(ReferenceInEnglish) {
    SetLanguage(L"en");
    auto russian = [](const std::string& s) { return s.find('\xD0') != std::string::npos || s.find('\xD1') != std::string::npos; };
    std::vector<std::string> words = Hints::KnownInstructions();
    for (const char* w : {"vaddps", "vpaddd", "rax", "r12d", "xmm3", "ymm9", "rip", "db", "resq", "equ", "section",
                          "%macro", "%ifdef", "default", "jnz", "setge", "cmovb"}) {
        words.push_back(w);
    }
    int left = 0;
    for (const std::string& w : words) {
        std::string card = Hints::For(w);
        if (card.empty() || russian(card)) {
            if (left++ < 10) printf("    not in English: %s\n", w.c_str());
        }
    }
    for (const char* f : {"printf", "WriteFile", "CreateFileA", "ExitProcess"}) {
        std::string card = Hints::Function(f);
        if (card.empty() || russian(card)) {
            if (left++ < 10) printf("    not in English: %s\n", f);
        }
    }
    SetLanguage(L"ru");
    CHECK_EQ(left, 0);
    CHECK(russian(Hints::For("mov")));
}

TEST(ExplainLine) {
    std::map<std::string, uint64_t> v = {{"rax", 5}, {"rbx", 3}, {"rcx", 1}, {"rdx", 0}, {"eax", 5}, {"qword [counter]", 7},
                                         {"qword [rsp]", 0x401000}, {"10", 10}, {"3", 3}, {"counter", 0x404000}};
    Analyzer::Eval eval = [&](const std::string& e) -> std::optional<uint64_t> {
        auto it = v.find(e);
        return it == v.end() ? std::nullopt : std::optional<uint64_t>(it->second);
    };
    CHECK_EQ(Analyzer::ExplainLine("    add rax, rbx", eval, 0), std::string("rax = 5 + 3 → 8"));
    CHECK_EQ(Analyzer::ExplainLine("    mov rax, 10   ; comment", eval, 0), std::string("rax = 10"));
    CHECK_EQ(Analyzer::ExplainLine(".l: inc qword [counter]", eval, 0), std::string("[counter] = 7 + 1 → 8"));
    CHECK_EQ(Analyzer::ExplainLine("    sub rbx, 10", eval, 0), std::string("rbx = 3 − 10 → -7"));
    CHECK_EQ(Analyzer::ExplainLine("    xor eax, eax", eval, 0), std::string("eax = 0 (обнуление)"));
    CHECK_EQ(Analyzer::ExplainLine("    cmp rbx, 10", eval, 0), std::string("сравнение: 3 < 10"));
    CHECK_EQ(Analyzer::ExplainLine("    div rbx", eval, 0), std::string("5 / 3: rax = 1, rdx = 2"));
    CHECK_EQ(Analyzer::ExplainLine("    lea rdx, [counter]", eval, 0), std::string("rdx = адрес 0x404000"));
    std::string jl = Analyzer::ExplainLine("    jl .loop", eval, 1 << 7);  // SF = 1, OF = 0
    CHECK(jl.find("SF ≠ OF") != std::string::npos && jl.find("переход на .loop") != std::string::npos);
    CHECK(Analyzer::ExplainLine("    jge .loop", eval, 1 << 7).find("перехода не будет") != std::string::npos);
    CHECK_EQ(Analyzer::ExplainLine("    loop .l", eval, 0), std::string("rcx = 1 − 1 → выход из цикла"));
    CHECK_EQ(Analyzer::ExplainLine("    ret", eval, 0), std::string("возврат по адресу 0x401000"));
    CHECK_EQ(Analyzer::ExplainLine("; only a comment", eval, 0), std::string());
    CHECK_EQ(Analyzer::ExplainLine("    mov rax, [unknown]", eval, 0), std::string());
}

TEST(StructsAndWatchForms) {
    auto s = Structs::Parse("struc Point\n    .x resd 1\n    .y: resd 1\n    .name resb 16\nendstruc\n"
                            "struc Pair\n.a dq 0\n.b dw 1, 2\nendstruc\n");
    CHECK_EQ(s.size(), 2u);
    CHECK_EQ(s["Point"].size, 24);
    CHECK(s["Point"].fields.size() == 3 && s["Point"].fields[1].name == "y" && s["Point"].fields[1].offset == 4);
    CHECK(s["Point"].fields[2].size == 1 && s["Point"].fields[2].count == 16);
    CHECK_EQ(s["Pair"].size, 12);
    auto as = Structs::SplitAs("[rbx + 8] as Point");
    CHECK(as && as->first == "[rbx + 8]" && as->second == "Point");
    CHECK(!Structs::SplitAs("rax"));
    auto dump = Structs::SplitDump("msg, 32");
    CHECK(dump && dump->first == "msg" && dump->second == 32);
    CHECK(!Structs::SplitDump("[a, 3]"));
    CHECK(!Structs::SplitDump("x, 1000"));
}

TEST(UpdateReleaseAndVersions) {
    std::string json = R"({"url":"x","html_url":"https://github.com/Anready/AsmEditor/releases/tag/v1.3.0",
        "tag_name":"v1.3.0","body":"Новое:\n- fixes \"quoted\"",
        "assets":[{"name":"AsmEditor-portable.zip","digest":"sha256:aaaa","browser_download_url":
        "https://github.com/Anready/AsmEditor/releases/download/v1.3.0/AsmEditor-portable.zip"},
        {"name":"AsmEditor.exe","size":5,"digest":"sha256:0123abcd","browser_download_url":
        "https://github.com/Anready/AsmEditor/releases/download/v1.3.0/AsmEditor.exe"},
        {"name":"AsmEditor-setup-1.3.0.exe","browser_download_url":
        "https://github.com/Anready/AsmEditor/releases/download/v1.3.0/AsmEditor-setup-1.3.0.exe"}]})";
    auto r = Update::ParseRelease(json);
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->version, std::string("1.3.0"));
        CHECK_EQ(r->notes, std::string("Новое:\n- fixes \"quoted\""));
        CHECK_EQ(r->exeUrl, std::string("https://github.com/Anready/AsmEditor/releases/download/v1.3.0/AsmEditor.exe"));
        CHECK_EQ(r->exeSha256, std::string("0123abcd"));
        CHECK(r->setupUrl.find("AsmEditor-setup-1.3.0.exe") != std::string::npos && r->setupSha256.empty());
    }
    CHECK(!Update::ParseRelease("{\"message\":\"Not Found\"}"));
    CHECK(Update::CompareVersions("1.2.0", "1.2.1") < 0);
    CHECK(Update::CompareVersions("1.10.0", "1.9.3") > 0);
    CHECK(Update::CompareVersions("1.2", "1.2.0") == 0);
    CHECK(Update::CompareVersions(ASMEDITOR_VERSION_TEXT, "1.0.0") > 0);
}

TEST(UpdateFilesAndDownload) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path dir = fs::path(tmp) / L"asmeditor-tests" / L"update";
    fs::create_directories(dir);
    fs::path abc = dir / L"abc.txt";
    std::ofstream(abc, std::ios::binary | std::ios::trunc) << "abc";
    CHECK_EQ(Update::Sha256(abc), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // replacing a "running" exe: the old one is kept as .old, removed by CleanupOld
    fs::path exe = dir / L"app.exe", fresh = dir / L"app.new";
    std::ofstream(exe, std::ios::binary | std::ios::trunc) << "old";
    std::ofstream(fresh, std::ios::binary | std::ios::trunc) << "new";
    std::string error;
    CHECK(Update::ReplaceExecutable(exe, fresh, error));
    std::ifstream in(exe, std::ios::binary);
    std::string now((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK_EQ(now, std::string("new"));
    CHECK(fs::exists(dir / L"app.exe.old"));
    Update::CleanupOld(exe);
    CHECK(!fs::exists(dir / L"app.exe.old"));
    // Verify: a real program with its checksum passes, a wrong checksum or a text file does not
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    CHECK(Update::Verify(self, Update::Sha256(self), error));
    CHECK(!Update::Verify(self, "00", error));
    CHECK(!Update::Verify(abc, "", error));
    // HTTPS download and the GitHub API (skipped without a network)
    fs::path readme = dir / L"README.md";
    if (Update::Download(L"https://raw.githubusercontent.com/Anready/AsmEditor/main/README.md", readme, error)) {
        std::ifstream r(readme, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(r)), std::istreambuf_iterator<char>());
        CHECK(text.find("AsmEditor") != std::string::npos);
        auto latest = Update::Latest(error);
        printf("    latest release: %s%s\n", latest ? latest->tag.c_str() : "(none)", error.empty() ? "" : (" — " + error).c_str());
        CHECK(latest || error.empty());  // either a release, or none published yet
    } else {
        printf("    (network checks skipped: %s)\n", error.c_str());
    }
}

TEST(LinuxPlatformsNasmArgs) {
    Settings s;
    s.nasmArgs = L"-f win64 -g -F cv8";
    RunConfig c;
    c.platform = L"wsl";
    CHECK_EQ(Toolchain::NasmArgs(s, c), std::wstring(L"-f elf64 -g -F dwarf"));
    c.platform = L"wsl32";
    CHECK(c.Wsl() && c.Linux32());
    CHECK_EQ(Toolchain::NasmArgs(s, c), std::wstring(L"-f elf32 -g -F dwarf"));
}

TEST(GdbMiRecords) {
    auto r = Mi::Parse("12^done,bkpt={number=\"3\",type=\"breakpoint\",file=\"main.asm\",line=\"13\"}");
    CHECK(r && r->type == '^' && r->token == 12 && r->klass == "done");
    if (r) {
        const Mi::Value* b = r->results.Get("bkpt");
        CHECK(b && b->Str("number") == "3" && b->Str("line") == "13");
    }
    r = Mi::Parse("*stopped,reason=\"breakpoint-hit\",disp=\"keep\",bkptno=\"1\",frame={addr=\"0x401005\",func=\"main\","
                  "args=[],file=\"main.asm\",fullname=\"/mnt/d/proj/main.asm\",line=\"13\"},thread-id=\"1\"");
    CHECK(r && r->type == '*' && r->klass == "stopped" && r->results.Str("reason") == "breakpoint-hit");
    if (r) {
        const Mi::Value* f = r->results.Get("frame");
        CHECK(f && f->Str("fullname") == "/mnt/d/proj/main.asm" && f->Get("args") && f->Get("args")->kind == Mi::Value::Kind::List);
    }
    r = Mi::Parse("5^done,register-names=[\"rax\",\"rbx\",\"\",\"rcx\"]");
    CHECK(r && r->results.Get("register-names") && r->results.Get("register-names")->items.size() == 4);
    r = Mi::Parse("6^done,stack=[frame={level=\"0\",addr=\"0x401010\",func=\"helper\"},frame={level=\"1\",func=\"main\"}]");
    CHECK(r && r->results.Get("stack") && r->results.Get("stack")->fields.size() == 2);
    r = Mi::Parse("~\"Line 13 of \\\"main.asm\\\" starts at address 0x401005\\n\"");
    CHECK(r && r->type == '~' && r->text == "Line 13 of \"main.asm\" starts at address 0x401005\n");
    CHECK(!Mi::Parse("(gdb) "));
    CHECK(!Mi::Parse("hello from the program"));
    CHECK_EQ(Mi::FromWsl("/mnt/d/proj/main.asm").wstring(), std::wstring(L"D:\\proj\\main.asm"));
    CHECK_EQ(Mi::FromWsl("/home/u/a.asm").wstring(), std::wstring(L"/home/u/a.asm"));
    CHECK_EQ(Mi::GdbExpression("rcx == 5 && dword [counter] > 3"),
             std::string("$rcx == 5 && *(unsigned int*)(counter) > 3"));
    CHECK_EQ(Mi::GdbExpression("[rsp+8]"), std::string("*(long*)($rsp+8)"));
    auto [format, args] = Mi::LogFormat("i={rbx} c={[counter]:x} 100%");
    CHECK_EQ(format, std::string("i=%ld c=%#lx 100%%"));
    CHECK(args.size() == 2 && args[0] == "(long)($rbx)" && args[1] == "(long)(*(long*)(counter))");
    CHECK_EQ(Mi::Quote("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));
    CHECK_EQ(Mi::ParseNumber("(long *) 0x404010 <counter>"), 0x404010ull);
    CHECK_EQ(Mi::ParseNumber("value 42"), 42ull);
}

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

// Every Tr("…") with a literal Russian text in the sources has an English translation.
TEST(EveryUiTextTranslated) {
    fs::path srcDir = fs::path(__FILE__).parent_path().parent_path() / L"src";
    SetLanguage(L"en");
    int missing = 0;
    for (const auto& entry : fs::directory_iterator(srcDir)) {
        if (entry.path().extension() != L".cpp") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // Tr("…") and Format("… %s …", arg) of the analyzer
        for (const std::string needle : {"Tr(", "Format("})
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
            if (at > 0 && (isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_')) continue;
            size_t i = at + needle.size();
            bool format = needle[0] == 'F';
            bool wide = false;
            if (text.compare(i, 12, "std::string(") == 0) i += 12;
            std::string value;
            bool any = false;
            for (;;) {  // adjacent literals: "a" "b"
                while (i < text.size() && isspace(static_cast<unsigned char>(text[i]))) i++;
                if (text[i] == 'L' && text[i + 1] == '"') {
                    wide = true;
                    i++;
                }
                if (text[i] != '"') break;
                any = true;
                for (i++; i < text.size() && text[i] != '"'; i++) {
                    if (text[i] == '\\') {
                        char e = text[++i];
                        value += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    } else {
                        value += text[i];
                    }
                }
                i++;
            }
            while (i < text.size() && isspace(static_cast<unsigned char>(text[i]))) i++;
            if (!any || (text[i] != ')' && !(format && text[i] == ','))) continue;  // not a plain literal
            if (value.find('\xD0') == std::string::npos && value.find('\xD1') == std::string::npos) continue;
            bool ok = wide ? std::wstring(Tr(Widen(value).c_str())) != Widen(value) : Tr(value) != value;
            if (!ok) {
                missing++;
                printf("    untranslated (%s): %s\n", Narrow(entry.path().filename().wstring()).c_str(), value.c_str());
            }
        }
    }
    for (const Exercises::Exercise& e : Exercises::All()) {
        if (std::wstring(Tr(e.title.c_str())) == e.title) {
            missing++;
            printf("    untranslated task title: %s\n", Narrow(e.title).c_str());
        }
        for (const std::string& h : e.hints) {
            if (Tr(h) == h) {
                missing++;
                printf("    untranslated hint: %s\n", h.c_str());
            }
        }
    }
    for (const Conditions::Condition& c : Conditions::All(0)) {
        if (Tr(c.meaning) == c.meaning) {
            missing++;
            printf("    untranslated condition: %s\n", c.meaning.c_str());
        }
    }
    SetLanguage(L"ru");
    CHECK_EQ(missing, 0);
}

// Registers under a test key (not the real HKCU\Software\Classes) and removes it again.
TEST(FileAssociationInTestKey) {
    const std::wstring classes = L"Software\\AsmEditorTests\\Classes";
    fs::path exe = L"C:\\Tools\\AsmEditor\\AsmEditor.exe";
    std::wstring error;
    CHECK(!FileAssociation::IsRegistered(exe, classes));
    CHECK(FileAssociation::Register(exe, error, classes));
    CHECK(FileAssociation::IsRegistered(exe, classes));
    wchar_t buf[256];
    DWORD size = sizeof(buf);
    CHECK(RegGetValueW(HKEY_CURRENT_USER, (classes + L"\\.asm").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS &&
          std::wstring(buf) == L"AsmEditor.asm");
    size = sizeof(buf);
    CHECK(RegGetValueW(HKEY_CURRENT_USER, (classes + L"\\AsmEditor.asm\\shell\\open\\command").c_str(), nullptr, RRF_RT_REG_SZ,
                       nullptr, buf, &size) == ERROR_SUCCESS &&
          std::wstring(buf) == L"\"C:\\Tools\\AsmEditor\\AsmEditor.exe\" \"%1\"");
    FileAssociation::Unregister(classes);
    CHECK(!FileAssociation::IsRegistered(exe, classes));
    size = sizeof(buf);
    CHECK(RegGetValueW(HKEY_CURRENT_USER, (classes + L"\\.asm").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\AsmEditorTests");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\AsmEditorTests");
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

// ---- built-in disassembler: round trip through NASM -------------------------------------------------------------

// Assembles `source` with NASM (-f bin, no optimization) and returns the bytes.
std::optional<std::vector<uint8_t>> Assemble(const Settings& s, const std::string& source, const std::string& name) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path dir = fs::path(tmp) / L"asmeditor-tests" / L"disasm";
    fs::create_directories(dir);
    fs::path src = dir / Widen(name + ".asm"), bin = dir / Widen(name + ".bin");
    std::ofstream(src, std::ios::binary) << source;
    std::error_code ec;
    fs::remove(bin, ec);
    std::string log;
    int code = Toolchain::Exec({s.nasmPath, L"-f", L"bin", L"-O0", src.wstring(), L"-o", bin.wstring()}, dir, nullptr, &log);
    if (code != 0) {
        printf("    nasm: %s\n", log.c_str());
        return std::nullopt;
    }
    std::ifstream in(bin, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Every instruction of the corpus is decoded (no "db"), and its text, assembled again at the same address,
// decodes to the same text.
void RoundTrip(int bits, const std::vector<std::string>& corpus) {
    Settings s;
    s.Load();
    Toolchain::ApplyDefaults(s);
    if (!Toolchain::NasmAvailable(s)) {
        printf("    (skipped: needs NASM)\n");
        return;
    }
    const uint64_t org = bits == 64 ? 0x401000 : 0x8048000;
    std::string source = "bits " + std::to_string(bits) + "\norg " + std::to_string(org) + "\n";
    for (const std::string& line : corpus) source += line + "\n";
    auto bytes = Assemble(s, source, "corpus" + std::to_string(bits));
    CHECK(bytes.has_value());
    if (!bytes) return;
    auto lines = Disasm::Decode(bytes->data(), bytes->size(), org, bits);
    CHECK_EQ(lines.size(), corpus.size());
    int bad = 0;
    std::string batch;
    for (size_t k = 0; k < lines.size(); k++) {
        const Disasm::Line& l = lines[k];
        if (l.text.rfind("db ", 0) == 0) {
            if (bad++ < 15) printf("    not decoded: %s  (%s)\n", k < corpus.size() ? corpus[k].c_str() : "?", l.bytes.c_str());
            continue;
        }
        // Each instruction alone at its address (relative jumps and [rel] need the same address). NASM makes
        // [rel number] absolute in a flat binary: give it as $$ + offset, which it encodes RIP-relative.
        std::string text = l.text;
        if (size_t r = text.find("[rel 0x"); r != std::string::npos) {
            size_t end = text.find(']', r);
            uint64_t target = strtoull(text.c_str() + r + 5, nullptr, 16);
            text.replace(r, end - r + 1, "[rel $$+" + std::to_string(target - l.address) + "]");
        }
        auto again = Assemble(s, "bits " + std::to_string(bits) + "\norg " + std::to_string(l.address) + "\n" + text + "\n", "one");
        if (!again) {
            if (bad++ < 15) printf("    NASM rejects: %s\n", l.text.c_str());
            continue;
        }
        auto redecoded = Disasm::Decode(again->data(), again->size(), l.address, bits);
        if (redecoded.size() != 1 || redecoded[0].text != l.text) {
            if (bad++ < 15) printf("    round trip: %s -> %s\n", l.text.c_str(), redecoded.empty() ? "" : redecoded[0].text.c_str());
        }
    }
    CHECK_EQ(bad, 0);
}

TEST(DisassemblerRoundTrip64) {
    RoundTrip(64, {
        "mov rax, rbx", "mov eax, ecx", "mov ax, dx", "mov al, bl", "mov sil, dil", "mov r8, r15", "mov r12d, r13d",
        "mov rax, [rbx]", "mov rax, [rbx+8]", "mov [rbp-16], rcx", "mov rdx, [rax+rcx*4+0x100]", "mov rax, [rsp+8]",
        "mov rax, [r13]", "mov rax, [r12+r9*8-4]", "mov qword [rax], 5", "mov dword [rbx], -1", "mov byte [rcx], 0x41",
        "mov word [rdx], 0x1234", "mov rax, 0x123456789abc", "mov eax, 7", "mov r9b, 3", "mov rax, [rel $+0x100]",
        "lea rcx, [rel $+0x20]", "lea rax, [rbx+rcx*2+3]", "movzx eax, byte [rsi]", "movzx ecx, word [rdi+2]",
        "movsx rax, al", "movsxd rax, dword [rbx]", "movsxd rcx, edx",
        "add rax, rbx", "add rax, 1", "add rax, -1", "add eax, 0x12345678", "add al, 5", "add rsp, 40", "sub rsp, 0x28",
        "sub eax, ecx", "adc rdx, 0", "sbb rax, rbx", "and rax, -16", "or ecx, 0x80", "xor eax, eax", "xor r10, r11",
        "cmp rax, 10", "cmp byte [rsi], 0", "cmp rcx, [rdx]", "test rax, rax", "test al, 1", "test dword [rbx], 0x100",
        "inc rax", "dec ecx", "inc qword [rbx]", "dec byte [rax]", "neg rax", "not rdx", "mul rcx", "imul rbx",
        "div rcx", "idiv qword [rsp+8]", "imul rax, rbx", "imul rax, rbx, 10", "imul ecx, edx, 1000",
        "shl rax, 3", "shr rdx, 1", "sar eax, cl", "rol rbx, 4", "ror ecx, 1", "rcl rax, 1", "shld rax, rbx, 4",
        "shrd rax, rbx, cl", "bt rax, 3", "bts rax, rbx", "btr ecx, 5", "btc qword [rax], 1", "bsf rax, rbx",
        "bsr ecx, edx", "popcnt rax, rbx", "lzcnt eax, ecx", "tzcnt rdx, r8", "bswap rax", "bswap r12d",
        "push rbx", "push r12", "pop rbp", "pop r15", "push 10", "push 0x12345", "push qword [rax]", "pop qword [rbx]",
        "call $+0x100", "call rax", "call qword [rel $+0x40]", "jmp $+0x200", "jmp short $+0x10", "jmp rcx",
        "jmp qword [rax+8]", "je $+0x10", "jne $-0x20", "jl $+0x1000", "jg $+5", "jb $+10", "jae $+10", "js $+3",
        "jo $+4", "jp $+6", "loop $-5", "jrcxz $+8", "ret", "ret 16", "leave", "enter 32, 0", "nop", "pause",
        "int3", "int 0x80", "hlt", "syscall", "ud2", "cpuid", "rdtsc", "cqo", "cdq", "cdqe", "cwde", "cbw", "cwd",
        "pushfq", "popfq", "sahf", "lahf", "clc", "stc", "cmc", "cld", "std",
        "sete al", "setne bl", "setg cl", "setb byte [rax]", "cmove rax, rbx", "cmovl ecx, edx", "cmovge rax, [rbx]",
        "xchg rax, rbx", "xchg rcx, rax", "xchg [rbx], eax", "cmpxchg [rcx], rdx", "lock cmpxchg [rcx], rdx",
        "xadd [rax], ebx", "lock add qword [rax], 1", "lock inc dword [rbx]",
        "movsb", "movsq", "rep movsb", "rep stosq", "lodsb", "repne scasb", "repe cmpsb", "stosd",
        "nop dword [rax+rax+0]",
        "movss xmm0, [rax]", "movsd xmm1, [rbx+8]", "movsd [rcx], xmm2", "movss xmm3, xmm4", "movaps xmm0, xmm1",
        "movups xmm5, [rdx]", "movapd [rax], xmm6", "movdqa xmm7, [rsp]", "movdqu [rbx], xmm8", "movq xmm0, rax",
        "movq rax, xmm1", "movd xmm2, ecx", "movd edx, xmm3", "movq xmm4, [rax]", "movq [rbx], xmm5",
        "addsd xmm0, xmm1", "subss xmm2, [rax]", "mulpd xmm3, xmm4", "divps xmm5, [rbx]", "sqrtsd xmm0, xmm0",
        "minsd xmm1, xmm2", "maxss xmm3, xmm4", "xorps xmm0, xmm0", "andpd xmm1, [rax]", "orps xmm2, xmm3",
        "cvtsi2sd xmm0, rax", "cvtsi2ss xmm1, dword [rbx]", "cvttsd2si rax, xmm0", "cvtsd2si ecx, xmm1",
        "cvtss2sd xmm0, xmm1", "cvtsd2ss xmm2, [rax]", "ucomisd xmm0, xmm1", "comiss xmm2, [rax]",
        "pxor xmm0, xmm0", "paddd xmm1, xmm2", "psubq xmm3, [rax]", "pcmpeqb xmm4, xmm5", "pand xmm6, xmm7",
        "por xmm8, xmm9", "pmovmskb eax, xmm1", "pshufd xmm0, xmm1, 0x1b", "punpcklbw xmm2, xmm3",
        // x87
        "fld dword [rax]", "fld qword [rbx+8]", "fld tword [rcx]", "fld st1", "fild dword [rax]", "fild qword [rax]",
        "fild word [rax]", "fst qword [rax]", "fstp dword [rbx]", "fstp tword [rcx]", "fstp st1", "fist dword [rax]",
        "fistp qword [rax]", "fisttp qword [rax]", "fadd dword [rax]", "fadd qword [rax]", "fadd st0, st2",
        "fadd st3, st0", "faddp st1, st0", "fsub qword [rax]", "fsub st0, st1", "fsub st1, st0", "fsubr st1, st0",
        "fsubp st1, st0", "fsubrp st1, st0", "fmul st0, st1", "fmulp st2, st0", "fdiv dword [rax]",
        "fdiv st1, st0", "fdivr st1, st0", "fdivp st1, st0", "fdivrp st1, st0", "fiadd dword [rax]",
        "fimul word [rax]", "fcom st1", "fcomp qword [rax]", "fcompp", "fucomi st0, st1", "fcomip st0, st2",
        "fucompp", "fxch st1", "fchs", "fabs", "fsqrt", "fsin", "fcos", "fsincos", "fptan", "fpatan", "fprem",
        "frndint", "fscale", "fld1", "fldz", "fldpi", "fldl2e", "fninit", "fnclex", "fnstsw ax",
        "fnstcw word [rax]", "fldcw word [rax]", "ffree st3", "fcmovb st0, st1", "fcmovne st0, st2", "wait", "ftst", "fxam",
        // AVX / AVX2 / FMA
        "vmovaps ymm0, ymm1", "vmovups ymm2, [rax]", "vmovups [rbx], ymm3", "vmovapd xmm4, [rcx]", "vmovdqa ymm5, [rdx]",
        "vmovdqu [rax], ymm6", "vmovss xmm0, [rax]", "vmovsd [rbx], xmm1", "vmovss xmm0, xmm1, xmm2",
        "vaddps ymm0, ymm1, ymm2", "vaddpd xmm3, xmm4, [rax]", "vaddss xmm0, xmm1, xmm2", "vaddsd xmm0, xmm1, [rax]",
        "vsubps ymm8, ymm9, ymm10", "vmulpd ymm0, ymm1, [rax+32]", "vdivss xmm1, xmm2, xmm3", "vmaxps ymm0, ymm1, ymm2",
        "vsqrtps ymm0, ymm1", "vsqrtsd xmm0, xmm1, xmm2", "vxorps ymm0, ymm0, ymm0", "vandpd xmm1, xmm2, xmm3",
        "vpxor ymm0, ymm1, ymm2", "vpaddd ymm3, ymm4, [rax]", "vpsubq xmm5, xmm6, xmm7", "vpcmpeqb ymm1, ymm2, ymm3",
        "vpmulld ymm0, ymm1, ymm2", "vbroadcastss ymm0, [rax]", "vpbroadcastd ymm1, xmm2", "vzeroupper", "vzeroall",
        "vfmadd231ps ymm0, ymm1, ymm2", "vfmadd213sd xmm0, xmm1, [rax]", "vfmsub132pd ymm3, ymm4, ymm5",
        "vfnmadd231ss xmm1, xmm2, xmm3",
    });
}

TEST(DisassemblerRoundTrip32) {
    RoundTrip(32, {
        "mov eax, ebx", "mov ax, cx", "mov al, ah", "mov eax, [ebx+8]", "mov [ebp-4], ecx", "mov dword [eax], 5",
        "mov eax, [0x804a000]", "lea esi, [eax+ecx*4]", "push ebx", "push dword [esp+4]", "pop edi", "push 0x10",
        "inc eax", "dec ecx", "add esp, 8", "sub esp, 0x20", "call $+0x50", "call eax", "jmp $-10", "je $+0x30",
        "ret", "pushad", "popad", "cdq", "idiv ecx", "imul eax, [ebx], 3", "shl eax, 2", "test eax, eax",
        "movzx eax, byte [esi]", "rep movsd", "loop $-3", "jecxz $+5", "int 0x80", "movsd xmm0, [eax]",
        "cvtsi2sd xmm1, eax", "pushfd", "popfd", "xchg eax, edx", "cmovne eax, ebx", "setl al",
    });
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
    std::vector<std::string> output;  // OutputDebugString and logpoint lines
    std::vector<ProfileLine> profile;

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
                } else if (msg.message == WM_DEBUG_ODS) {
                    std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(msg.lParam));
                    output.push_back(*s);
                } else if (msg.message == WM_DEBUG_PROFILE) {
                    std::unique_ptr<std::vector<ProfileLine>> p(reinterpret_cast<std::vector<ProfileLine>*>(msg.lParam));
                    profile = *p;
                } else if (msg.message == WM_DEBUG_LOG) {
                    delete reinterpret_cast<std::string*>(msg.lParam);
                }
            }
            if (stops.size() > before || exited) return true;
            Sleep(5);
        }
        return false;
    }
};

std::optional<fs::path> BuildProgram(const std::string& name, const std::string& source, bool x86 = false) {
    Settings s;
    s.Load();
    Toolchain::ApplyDefaults(s);
    if (x86) {  // the "MSVC link x86" preset, as the settings dialog sets it up
        auto msvc = Toolchain::FindMsvc(true);
        if (!msvc) {
            printf("    (skipped: needs the 32-bit MSVC tools)\n");
            return std::nullopt;
        }
        s.linkerPreset = L"MSVC_X86_CRT";
        s.linkerPath = msvc->link.wstring();
        s.linkerArgs = FindPreset(L"MSVC_X86_CRT")->args;
        s.libPaths.clear();
        for (const fs::path& p : msvc->libPaths) s.libPaths.push_back(p.wstring());
        s.nasmArgs = L"-f win32 -g -F cv8";
    }
    if (!Toolchain::NasmAvailable(s) || !Toolchain::LinkerAvailable(s) ||
        s.linkerPreset != (x86 ? L"MSVC_X86_CRT" : L"MSVC_CRT")) {
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

// A logpoint prints on every pass and does not stop; a condition in front of "log:" filters the passes.
TEST(DebuggerLogpoints) {
    auto exe = BuildProgram("counter", kCounterProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    Debugger::Breakpoints bps{{Debugger::Key(src),
                               {{13, "log: rbx={rbx} c={qword [counter]:x}"}, {24, "rbx >= 4 log: helper {rbx}"}, {20, ""}}}};
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, bps, false, std::nullopt, error));
    CHECK(host.Wait(10000));
    if (host.stops.empty()) return;
    CHECK_EQ(host.stops.size(), 1u);
    CHECK_EQ(host.stops.back()->location.line, 20);
    host.Wait(200);
    std::vector<std::string> lines;
    for (const auto& s : host.output) lines.push_back(s);
    CHECK_EQ(lines.size(), 7u);
    if (lines.size() == 7) {
        CHECK_EQ(lines[0], std::string("rbx=0 c=0x1\n"));
        CHECK_EQ(lines[3], std::string("rbx=3 c=0x4\n"));
        CHECK_EQ(lines[4], std::string("helper 4\n"));
        CHECK_EQ(lines[5], std::string("rbx=4 c=0x5\n"));
        CHECK_EQ(lines[6], std::string("helper 5\n"));
    }
    d.Stop();
    host.Wait(5000);
}

// Step back restores registers and the program's variables to the previous stop.
TEST(DebuggerStepBack) {
    auto exe = BuildProgram("counter", kCounterProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, {{Debugger::Key(src), {{12, ""}}}}, false, std::nullopt, error));
    CHECK(host.Wait(10000));
    if (host.stops.empty()) return;
    auto counter = [&] {
        auto v = d.Evaluate("qword [counter]", error);
        return v ? *v : 999;
    };
    CHECK_EQ(counter(), 0ull);
    d.SetBreakpoints(src, {});
    d.StepOver();  // -> 13, counter 1
    CHECK(host.Wait(10000));
    d.StepOver();  // -> 14, rbx 1
    CHECK(host.Wait(10000));
    CHECK_EQ(host.stops.back()->location.line, 14);
    CHECK_EQ(d.Context().Rbx, 1ull);
    CHECK(d.StepBack(error));
    CHECK(host.Wait(5000));
    CHECK(host.stops.back()->reason == DebugStop::Reason::StepBack);
    CHECK_EQ(host.stops.back()->location.line, 13);
    CHECK_EQ(d.Context().Rbx, 0ull);
    CHECK_EQ(counter(), 1ull);
    CHECK(d.StepBack(error));
    CHECK(host.Wait(5000));
    CHECK_EQ(host.stops.back()->location.line, 12);
    CHECK_EQ(counter(), 0ull);
    CHECK(!d.StepBack(error));  // nothing earlier
    // forward again from the restored state
    d.StepOver();
    CHECK(host.Wait(10000));
    CHECK_EQ(host.stops.back()->location.line, 13);
    CHECK_EQ(counter(), 1ull);
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK(host.exited);
    CHECK_EQ(host.exitCode, 0ul);
}

const char* kHeapProgram =
    "default rel\n"                // 1
    "extern malloc\n"              // 2
    "global main\n"                // 3
    "section .bss\n"               // 4
    "    p resq 1\n"               // 5
    "section .text\n"              // 6
    "main:\n"                      // 7
    "    sub rsp, 40\n"            // 8
    "    mov ecx, 64\n"            // 9
    "    call malloc\n"            // 10
    "    mov [p], rax\n"           // 11
    "    mov qword [rax], 1\n"     // 12
    "    mov qword [rax], 2\n"     // 13
    "    mov qword [rax], 3\n"     // 14
    "    add rsp, 40\n"            // 15
    "    xor eax, eax\n"           // 16
    "    ret\n";                   // 17

// Step back also rolls back memory from malloc.
TEST(DebuggerStepBackHeap) {
    auto exe = BuildProgram("heap", kHeapProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, {{Debugger::Key(src), {{13, ""}}}}, false, std::nullopt, error));
    CHECK(host.Wait(10000));
    if (host.stops.empty()) return;
    d.SetBreakpoints(src, {});
    auto value = [&] {
        auto v = d.Evaluate("qword [[p]]", error);
        return v ? *v : 999;
    };
    CHECK_EQ(value(), 1ull);
    d.StepOver();
    CHECK(host.Wait(10000));
    d.StepOver();
    CHECK(host.Wait(10000));
    CHECK_EQ(value(), 3ull);
    CHECK(d.StepBack(error));
    CHECK(host.Wait(5000));
    CHECK_EQ(value(), 2ull);
    CHECK(d.StepBack(error));
    CHECK(host.Wait(5000));
    CHECK_EQ(value(), 1ull);
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK(host.exited);
}

const char* kTickProgram =
    "default rel\n"              // 1
    "extern Sleep\n"             // 2
    "global main\n"              // 3
    "section .bss\n"             // 4
    "    ticks resq 1\n"         // 5
    "section .text\n"            // 6
    "main:\n"                    // 7
    "    sub rsp, 40\n"          // 8
    ".loop:\n"                   // 9
    "    inc qword [ticks]\n"    // 10
    "    mov ecx, 20\n"          // 11
    "    call Sleep\n"           // 12
    "    jmp .loop\n";           // 13

// Attach to a running program, stop on a breakpoint, detach: the program keeps running.
TEST(DebuggerAttachAndDetach) {
    auto exe = BuildProgram("tick", kTickProgram);
    if (!exe) return;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe->wstring() + L"\"";
    CHECK(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    if (!pi.hProcess) return;
    Sleep(300);
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    LaunchOptions launch;
    launch.attachPid = pi.dwProcessId;
    std::wstring error;
    CHECK(d.Start(launch, nullptr, {{Debugger::Key(src), {{11, ""}}}}, false, std::nullopt, error));
    CHECK(host.Wait(10000));
    CHECK(!host.stops.empty() && host.stops.back()->location.line == 11);
    CHECK(d.Attached());
    auto ticks = d.Evaluate("qword [ticks]", error);
    CHECK(ticks && *ticks > 0);
    d.Stop();  // detach
    CHECK(host.Wait(5000));
    CHECK(host.exited);
    Sleep(1000);  // long enough for a pending trap or int3 to kill it if the detach left one
    CHECK_EQ(WaitForSingleObject(pi.hProcess, 0), static_cast<DWORD>(WAIT_TIMEOUT));  // still running
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

const char* kWindowProgram =
    "default rel\n"                       // 1
    "extern GetModuleHandleA\n"           // 2
    "extern RegisterClassA\n"             // 3
    "extern CreateWindowExA\n"            // 4
    "extern PostMessageA\n"               // 5
    "extern GetMessageA\n"                // 6
    "extern DispatchMessageA\n"           // 7
    "extern PostQuitMessage\n"            // 8
    "extern DefWindowProcA\n"             // 9
    "global main\n"                       // 10
    "section .data\n"                     // 11
    "    cls db \"AsmTestWnd\", 0\n"      // 12
    "section .bss\n"                      // 13
    "    wc resb 72\n"                    // 14
    "    msg resb 48\n"                   // 15
    "    hits resq 1\n"                   // 16
    "    inst resq 1\n"                   // 17
    "section .text\n"                     // 18
    "main:\n"                             // 19
    "    sub rsp, 104\n"                  // 20
    "    xor ecx, ecx\n"                  // 21
    "    call GetModuleHandleA\n"         // 22
    "    mov [inst], rax\n"               // 23
    "    mov [wc+24], rax\n"              // 24
    "    lea rax, [wndproc]\n"            // 25
    "    mov [wc+8], rax\n"               // 26
    "    lea rax, [cls]\n"                // 27
    "    mov [wc+64], rax\n"              // 28
    "    lea rcx, [wc]\n"                 // 29
    "    call RegisterClassA\n"           // 30
    "    xor ecx, ecx\n"                  // 31
    "    lea rdx, [cls]\n"                // 32
    "    lea r8, [cls]\n"                 // 33
    "    xor r9d, r9d\n"                  // 34
    "    mov qword [rsp+32], 0\n"         // 35
    "    mov qword [rsp+40], 0\n"         // 36
    "    mov qword [rsp+48], 0\n"         // 37
    "    mov qword [rsp+56], 0\n"         // 38
    "    mov qword [rsp+64], -3\n"        // 39  HWND_MESSAGE: a message-only window
    "    mov qword [rsp+72], 0\n"         // 40
    "    mov rax, [inst]\n"               // 41
    "    mov [rsp+80], rax\n"             // 42
    "    mov qword [rsp+88], 0\n"         // 43
    "    call CreateWindowExA\n"          // 44
    "    mov rcx, rax\n"                  // 45
    "    mov edx, 0x400\n"                // 46  WM_USER
    "    xor r8d, r8d\n"                  // 47
    "    xor r9d, r9d\n"                  // 48
    "    call PostMessageA\n"             // 49
    ".loop:\n"                            // 50
    "    lea rcx, [msg]\n"                // 51
    "    xor edx, edx\n"                  // 52
    "    xor r8d, r8d\n"                  // 53
    "    xor r9d, r9d\n"                  // 54
    "    call GetMessageA\n"              // 55
    "    test eax, eax\n"                 // 56
    "    jle .done\n"                     // 57
    "    lea rcx, [msg]\n"                // 58
    "    call DispatchMessageA\n"         // 59
    "    jmp .loop\n"                     // 60
    ".done:\n"                            // 61
    "    add rsp, 104\n"                  // 62
    "    xor eax, eax\n"                  // 63
    "    ret\n"                           // 64
    "wndproc:\n"                          // 65
    "    cmp edx, 0x400\n"                // 66
    "    jne .def\n"                      // 67
    "    inc qword [hits]\n"              // 68
    "    sub rsp, 40\n"                   // 69
    "    xor ecx, ecx\n"                  // 70
    "    call PostQuitMessage\n"          // 71
    "    add rsp, 40\n"                   // 72
    "    xor eax, eax\n"                  // 73
    "    ret\n"                           // 74
    ".def:\n"                             // 75
    "    jmp DefWindowProcA\n";           // 76

// A program with a window: a breakpoint in its window procedure, called by user32 from DispatchMessage.
TEST(DebuggerWindowProcedure) {
    auto exe = BuildProgram("window", kWindowProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, {{Debugger::Key(src), {{68, ""}}}}, false, std::nullopt, error));
    CHECK(host.Wait(15000));
    if (host.stops.empty()) return;
    CHECK_EQ(host.stops.back()->location.line, 68);
    CHECK_EQ(d.Context().Rdx & 0xFFFFFFFF, 0x400ull);  // the message
    auto frames = d.CallStack();
    bool user32 = false, dispatch = false;
    for (const auto& f : frames) {
        user32 |= Lower(f.function).find(L"user32") != std::wstring::npos;
        dispatch |= f.location.line == 59;
    }
    CHECK(user32);    // called by Windows
    CHECK(dispatch);  // from DispatchMessageA in main's message loop
    for (const auto& f : frames) printf("    %s %d\n", Narrow(f.function).c_str(), f.location.line);
    d.SetBreakpoints(src, {});
    d.Continue();
    CHECK(host.Wait(15000));
    CHECK(host.exited);
    CHECK_EQ(host.exitCode, 0ul);
}

// The profiler counts how often each line ran and how many instructions it took.
TEST(DebuggerProfiler) {
    auto exe = BuildProgram("counter", kCounterProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    launch.profile = true;
    auto start = std::chrono::steady_clock::now();
    CHECK(d.Start(launch, host.pty, {}, false, std::nullopt, error));
    CHECK(host.Wait(20000));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    uint64_t traced = 0;
    for (const ProfileLine& p : host.profile) traced += p.instructions;
    printf("    profiled run: %lld ms, %llu instructions on source lines\n", static_cast<long long>(ms),
           static_cast<unsigned long long>(traced));
    CHECK(host.exited);
    CHECK(host.stops.empty());
    std::map<int, ProfileLine> byLine;
    for (const ProfileLine& p : host.profile) byLine[p.location.line] = p;
    CHECK_EQ(byLine[9].hits, 1ull);
    CHECK_EQ(byLine[12].hits, 5ull);
    CHECK_EQ(byLine[12].instructions, 5ull);
    CHECK_EQ(byLine[24].hits, 5ull);
    CHECK_EQ(byLine[19].hits, 1ull);
    CHECK(byLine[19].seconds > 0);  // printf, run untraced, is timed on its call line
    CHECK_EQ(host.exitCode, 0ul);
}

const char* kBusyProgram =
    "default rel\n"                // 1
    "extern Sleep\n"               // 2
    "global main\n"                // 3
    "section .text\n"              // 4
    "main:\n"                      // 5
    "    sub rsp, 40\n"            // 6
    "    mov rcx, 400000000\n"     // 7
    ".spin:\n"                     // 8
    "    dec rcx\n"                // 9
    "    jnz .spin\n"              // 10
    "    mov ecx, 300\n"           // 11
    "    call Sleep\n"             // 12
    "    add rsp, 40\n"            // 13
    "    xor eax, eax\n"           // 14
    "    ret\n";                   // 15

// The sampling profiler: the busy loop and the Sleep call (through kernel32, unwound by .pdata) get the time.
TEST(DebuggerSamplingProfiler) {
    auto exe = BuildProgram("busy", kBusyProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    launch.sample = true;
    auto start = std::chrono::steady_clock::now();
    CHECK(d.Start(launch, host.pty, {}, false, std::nullopt, error));
    CHECK(host.Wait(30000));
    CHECK(host.exited);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::map<int, ProfileLine> byLine;
    uint64_t samples = 0;
    for (const ProfileLine& p : host.profile) byLine[p.location.line] = p, samples += p.samples;
    printf("    run %lld ms, %llu samples; loop %.0f ms, Sleep line %.0f ms\n", static_cast<long long>(ms),
           static_cast<unsigned long long>(samples), (byLine[9].seconds + byLine[10].seconds) * 1000, byLine[12].seconds * 1000);
    CHECK(samples > 100);
    CHECK(byLine[9].samples + byLine[10].samples > 10);
    CHECK(byLine[12].seconds > 0.2);  // Sleep(300)
}

const char* kCounterProgram32 =
    "extern _printf\n"            // 1
    "global _main\n"              // 2
    "section .data\n"             // 3
    "    counter dd 0\n"          // 4
    "    fmt db \"%d\", 10, 0\n"  // 5
    "section .text\n"             // 6
    "_main:\n"                    // 7
    "    push ebx\n"              // 8
    "    xor ebx, ebx\n"          // 9
    ".loop:\n"                    // 10
    "    inc dword [counter]\n"   // 11
    "    inc ebx\n"               // 12
    "    call helper\n"           // 13
    "    cmp ebx, 5\n"            // 14
    "    jl .loop\n"              // 15
    "    push dword [counter]\n"  // 16
    "    push fmt\n"              // 17
    "    call _printf\n"          // 18
    "    add esp, 8\n"            // 19
    "    pop ebx\n"               // 20
    "    xor eax, eax\n"          // 21
    "    ret\n"                   // 22
    "helper:\n"                   // 23
    "    nop\n"                   // 24
    "    ret\n";                  // 25

// The same scenario on a 32-bit program (WOW64 contexts, 4-byte pointers, STATUS_WX86_* events).
TEST(Debugger32Bit) {
    auto exe = BuildProgram("counter32", kCounterProgram32, true);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    Debugger::Breakpoints bps{{Debugger::Key(src), {{14, "ebx == 3"}, {24, "#2"}}}};
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, bps, false, std::nullopt, error));
    CHECK(host.Wait(10000));
    if (host.stops.empty()) return;
    CHECK(d.Is32());
    CHECK_EQ(d.PointerSize(), 4);
    CHECK_EQ(host.stops.back()->location.line, 24);
    CHECK_EQ(d.Context().Rbx, 2ull);
    auto frames = d.CallStack();
    CHECK(frames.size() >= 2u);
    if (frames.size() >= 2) CHECK_EQ(frames[1].location.line, 13);
    // step out of helper: back in main after the call
    d.StepOut();
    CHECK(host.Wait(10000));
    CHECK_EQ(host.stops.back()->location.line, 14);
    // the breakpoint on line 14 with ebx == 3
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK_EQ(host.stops.back()->location.line, 14);
    CHECK_EQ(d.Context().Rbx, 3ull);
    std::wstring err;
    auto counter = d.LookupSymbol("counter");
    CHECK(counter.has_value());
    if (counter) {
        CHECK_EQ(*counter >> 32, 0ull);
        DataWatch w;
        w.expression = "counter";
        w.address = *counter;
        w.size = 4;
        CHECK(d.AddDataWatch(w, err));
        d.Continue();
        CHECK(host.Wait(10000));
        CHECK(host.stops.back()->reason == DebugStop::Reason::Watchpoint);
        CHECK_EQ(host.stops.back()->location.line, 12);
        auto v = d.Evaluate("dword [counter]", err);
        CHECK(v && *v == 4);
        d.RemoveDataWatch(0);
    }
    CHECK(d.SetRegister("ebx", 4));
    d.SetBreakpoints(src, {});
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK(host.exited);
    CHECK_EQ(host.exitCode, 0ul);
}

const char* kQsortProgram =
    "default rel\n"              // 1
    "extern qsort\n"             // 2
    "global main\n"              // 3
    "section .data\n"            // 4
    "    arr dq 3, 1, 2\n"       // 5
    "section .text\n"            // 6
    "main:\n"                    // 7
    "    sub rsp, 40\n"          // 8
    "    lea rcx, [arr]\n"       // 9
    "    mov edx, 3\n"           // 10
    "    mov r8d, 8\n"           // 11
    "    lea r9, [compare]\n"    // 12
    "    call qsort\n"           // 13
    "    add rsp, 40\n"          // 14
    "    xor eax, eax\n"         // 15
    "    ret\n"                  // 16
    "compare:\n"                 // 17
    "    mov rax, [rcx]\n"       // 18
    "    sub rax, [rdx]\n"       // 19
    "    ret\n";                 // 20

// A callback from the C runtime: the frames inside ucrtbase come from its unwind information (.pdata).
TEST(DebuggerCallStackThroughDll) {
    auto exe = BuildProgram("qsort", kQsortProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, {{Debugger::Key(src), {{18, ""}}}}, false, std::nullopt, error));
    CHECK(host.Wait(10000));
    if (host.stops.empty()) return;
    CHECK_EQ(host.stops.back()->location.line, 18);
    auto frames = d.CallStack();
    size_t mainAt = 0;
    for (size_t i = 1; i < frames.size(); i++) {
        if (frames[i].location.line == 13) {
            mainAt = i;
            break;
        }
    }
    CHECK(mainAt >= 2);  // compare <- qsort (one or more frames in ucrtbase) <- main
    for (size_t i = 1; i < mainAt; i++) {
        CHECK(Lower(frames[i].function).find(L"ucrtbase") != std::wstring::npos);
    }
    CHECK(frames.size() > mainAt + 1);  // and on into the C runtime startup
    for (const auto& f : frames) printf("    %s\n", Narrow(f.function).c_str());
    d.SetBreakpoints(src, {});
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK(host.exited);
}

const char* kThreadProgram =
    "default rel\n"                    // 1
    "extern CreateThread\n"            // 2
    "extern WaitForSingleObject\n"     // 3
    "extern Sleep\n"                   // 4
    "global main\n"                    // 5
    "section .data\n"                  // 6
    "    shared dq 0\n"                // 7
    "section .text\n"                  // 8
    "main:\n"                          // 9
    "    sub rsp, 56\n"                // 10
    "    xor ecx, ecx\n"               // 11
    "    xor edx, edx\n"               // 12
    "    lea r8, [worker]\n"           // 13
    "    xor r9d, r9d\n"               // 14
    "    mov qword [rsp+32], 0\n"      // 15
    "    mov qword [rsp+40], 0\n"      // 16
    "    call CreateThread\n"          // 17
    "    mov [rsp+48], rax\n"          // 18
    "    mov ecx, 100\n"               // 19
    "    call Sleep\n"                 // 20
    "    mov rcx, [rsp+48]\n"          // 21
    "    mov edx, -1\n"                // 22
    "    call WaitForSingleObject\n"   // 23
    "    add rsp, 56\n"                // 24
    "    xor eax, eax\n"               // 25
    "    ret\n"                        // 26
    "worker:\n"                        // 27
    "    sub rsp, 40\n"                // 28
    "    mov ecx, 600\n"               // 29
    "    call Sleep\n"                 // 30
    "    inc qword [shared]\n"         // 31
    "    nop\n"                        // 32
    "    add rsp, 40\n"                // 33
    "    xor eax, eax\n"               // 34
    "    ret\n";                       // 35

// A data breakpoint set while the main thread is stopped also fires in a thread that was already running.
TEST(DebuggerWatchInOtherThread) {
    auto exe = BuildProgram("thread", kThreadProgram);
    if (!exe) return;
    DebugHost host;
    Debugger d(host.hwnd);
    fs::path src = exe->parent_path().parent_path() / L"main.asm";
    std::wstring error;
    LaunchOptions launch;
    launch.exe = *exe;
    launch.cwd = exe->parent_path();
    CHECK(d.Start(launch, host.pty, {{Debugger::Key(src), {{21, ""}}}}, false, std::nullopt, error));
    CHECK(host.Wait(10000));
    if (host.stops.empty()) return;
    CHECK_EQ(host.stops.back()->location.line, 21);
    auto shared = d.LookupSymbol("shared");
    CHECK(shared.has_value());
    if (!shared) return;
    DataWatch w;
    w.expression = "shared";
    w.address = *shared;
    w.size = 8;
    CHECK(d.AddDataWatch(w, error));
    d.SetBreakpoints(src, {});
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK(host.stops.back()->reason == DebugStop::Reason::Watchpoint);
    CHECK_EQ(host.stops.back()->location.line, 32);
    d.RemoveDataWatch(0);
    d.Continue();
    CHECK(host.Wait(10000));
    CHECK(host.exited);
}

// ---- learning: lessons, new exercises (solved by reference solutions), results -----------------------------------

// Runs a console program with the text on stdin; its stdout (false: did not finish in time).
bool RunExe(const fs::path& exe, const std::string& input, std::string& output) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead, inWrite, outRead, outWrite;
    CreatePipe(&inRead, &inWrite, &sa, 0);
    CreatePipe(&outRead, &outWrite, &sa, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    std::wstring cwd = exe.parent_path().wstring();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }
    std::string in = input + "\n";
    DWORD n = 0;
    WriteFile(inWrite, in.data(), static_cast<DWORD>(in.size()), &n, nullptr);
    CloseHandle(inWrite);
    std::thread reader([&] {
        char buf[4096];
        DWORD got;
        while (ReadFile(outRead, buf, sizeof(buf), &got, nullptr) && got) output.append(buf, got);
    });
    bool finished = WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0;
    if (!finished) TerminateProcess(pi.hProcess, 1);
    reader.join();
    CloseHandle(outRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return finished;
}

std::string Replace(std::string text, const std::string& what, const std::string& with) {
    size_t at = text.find(what);
    if (at == std::string::npos) {
        printf("    (starter has no \"%s\")\n", what.c_str());
        return text;
    }
    return text.replace(at, what.size(), with);
}

// Turns "a\nb" into indented CRLF lines, as the starters are written.
std::string Code(const std::string& lines) {
    std::string out;
    std::istringstream in(lines);
    for (std::string l; std::getline(in, l);) out += (l.empty() || l[0] == '.' || l.back() == ':' ? l : "    " + l) + "\r\n";
    return out;
}

void CheckSolves(const std::string& id, const std::string& todo, const std::string& solution) {
    const Exercises::Exercise* ex = Exercises::Find(id);
    CHECK(ex != nullptr);
    if (!ex) return;
    std::string source = Replace(ex->starter, todo, Code(solution));
    CHECK(StackCheck::Analyze(ex->starter).empty());
    for (const std::string& p : Exercises::CheckRequirements(*ex, source)) {
        printf("    %s: %s\n", id.c_str(), p.c_str());
        CHECK(false);
    }
    CHECK(!ex->hints.empty());
    auto exe = BuildProgram("ex-" + id, source);
    if (!exe) return;
    for (const Exercises::Test& t : ex->tests) {
        std::string output;
        bool finished = RunExe(*exe, t.input, output);
        bool ok = finished && Exercises::OutputMatches(output, t.expected);
        if (!ok) printf("    %s: input [%s] expected [%s] got [%s]\n", id.c_str(), t.input.c_str(), t.expected.c_str(), output.c_str());
        CHECK(ok);
    }
}

TEST(NewExercisesAreSolvable) {
    CheckSolves("reverse", "    ; TODO: запишите в result слово задом наперёд (и 0 в конце)\r\n",
                "lea r8, [input]\nxor ecx, ecx\n.len:\ncmp byte [r8 + rcx], 0\nje .got\ninc rcx\njmp .len\n.got:\n"
                "lea r9, [result]\nxor edx, edx\n.copy:\ntest rcx, rcx\njz .done\ndec rcx\nmov al, [r8 + rcx]\n"
                "mov [r9 + rdx], al\ninc rdx\njmp .copy\n.done:\nmov byte [r9 + rdx], 0");
    CheckSolves("upper", "    ; TODO: замените строчные буквы в input на заглавные\r\n",
                "lea r8, [input]\n.next:\nmov al, [r8]\ntest al, al\njz .done\ncmp al, 'a'\njb .skip\ncmp al, 'z'\nja .skip\n"
                "sub al, 32\nmov [r8], al\n.skip:\ninc r8\njmp .next\n.done:");
    CheckSolves("array_sum",
                "    ; TODO: прочитайте n чисел в arr (rbx — номер элемента, он сохраняется при call)\r\n"
                "    ; TODO: сложите их в rdx\r\n    xor     edx, edx\r\n",
                "xor ebx, ebx\n.read:\ncmp rbx, [n]\njge .sum\nlea rcx, [fmt_in]\nlea rdx, [arr + rbx*8]\ncall scanf\ninc rbx\n"
                "jmp .read\n.sum:\nxor edx, edx\nxor ebx, ebx\n.add:\ncmp rbx, [n]\njge .print\nadd rdx, [arr + rbx*8]\n"
                "inc rbx\njmp .add\n.print:");
    CheckSolves("sort",
                "    ; TODO: прочитайте n чисел в arr, отсортируйте их\r\n"
                "    ; TODO: напечатайте каждое: lea rcx, [fmt_num] / mov rdx, [arr + rbx*8] / call printf\r\n",
                "xor ebx, ebx\n.read:\ncmp rbx, [n]\njge .sort\nlea rcx, [fmt_in]\nlea rdx, [arr + rbx*8]\ncall scanf\ninc rbx\n"
                "jmp .read\n.sort:\nmov r8, [n]\n.outer:\ndec r8\njle .print0\nxor ecx, ecx\n.inner:\ncmp rcx, r8\njge .outer\n"
                "mov rax, [arr + rcx*8]\nmov rdx, [arr + rcx*8 + 8]\ncmp rax, rdx\njle .noswap\nmov [arr + rcx*8], rdx\n"
                "mov [arr + rcx*8 + 8], rax\n.noswap:\ninc rcx\njmp .inner\n.print0:\nxor ebx, ebx\n.print:\ncmp rbx, [n]\n"
                "jge .end\nlea rcx, [fmt_num]\nmov rdx, [arr + rbx*8]\ncall printf\ninc rbx\njmp .print\n.end:");
    CheckSolves("gcd", "    ; TODO\r\n    mov     rax, rcx\r\n    ret\r\n",
                "test rdx, rdx\njnz .rec\nmov rax, rcx\nret\n.rec:\nsub rsp, 40\nmov rax, rcx\nmov r8, rdx\nxor edx, edx\n"
                "div r8\nmov rcx, r8\ncall gcd\nadd rsp, 40\nret");
    CheckSolves("binary", "print_bin:\r\n    ; TODO\r\n    ret\r\n",
                "print_bin:\npush rbx\nsub rsp, 32\nmov rbx, rcx\ncmp rbx, 2\njb .digit\nmov rcx, rbx\nshr rcx, 1\ncall print_bin\n"
                ".digit:\nlea rcx, [fmt_bit]\nmov rdx, rbx\nand rdx, 1\ncall printf\nadd rsp, 32\npop rbx\nret");
    CheckSolves("file",
                "    ; TODO: rax = fopen(name, mode_w); fprintf(rax, fmt_str, input); fclose\r\n"
                "    ; TODO: rax = fopen(name, mode_r); fscanf(rax, fmt_in, copy); fclose\r\n",
                "section .bss\nfp resq 1\nsection .text\nlea rcx, [name]\nlea rdx, [mode_w]\ncall fopen\nmov [fp], rax\n"
                "mov rcx, rax\nlea rdx, [fmt_str]\nlea r8, [input]\ncall fprintf\nmov rcx, [fp]\ncall fclose\n"
                "lea rcx, [name]\nlea rdx, [mode_r]\ncall fopen\nmov [fp], rax\nmov rcx, rax\nlea rdx, [fmt_in]\n"
                "lea r8, [copy]\ncall fscanf\nmov rcx, [fp]\ncall fclose");
}

// The instruction limit: an iterative Fibonacci fits, and the count is the profiler's.
TEST(ExerciseInstructionLimit) {
    const Exercises::Exercise* fib = Exercises::Find("fib");
    CHECK(fib && fib->maxInstructions > 0);
    if (!fib) return;
    std::string solution = Replace(fib->starter, "    ; TODO: вычислите ответ в rdx\r\n    mov     rdx, [n]\r\n",
                                   Code("mov rcx, [n]\nmov rax, 1\nmov rdx, 1\ncmp rcx, 2\njle .done\nsub rcx, 2\n.next:\n"
                                        "lea r8, [rax+rdx]\nmov rax, rdx\nmov rdx, r8\ndec rcx\njnz .next\n.done:"));
    auto exe = BuildProgram("fib", solution);
    if (!exe) return;
    std::string output;
    CHECK(RunExe(*exe, "50", output) && Exercises::OutputMatches(output, "12586269025"));
    auto count = Debugger::CountInstructions(*exe, fib->tests.back().input, 30000);
    CHECK(count.has_value());
    if (count) {
        printf("    fib(50): %llu instructions (limit %llu)\n", static_cast<unsigned long long>(*count),
               static_cast<unsigned long long>(fib->maxInstructions));
        CHECK(*count > 200 && *count <= fib->maxInstructions);
    }
    auto counter = BuildProgram("counter", kCounterProgram);
    if (counter) CHECK_EQ(Debugger::CountInstructions(*counter, "", 30000).value_or(0), 43ull);
}

// A teacher's task file: parsed, its starter builds, a solution passes its tests.
TEST(TeacherTaskFile) {
    std::string file =
        "# tasks of group 101\n"
        "[task sum3]\n"
        "title = Сумма трёх чисел\n"
        "title.en = Sum of three numbers\n"
        "description = Прочитайте три числа и напечатайте их сумму.\n"
        "description.en = Read three numbers and print their sum.\n"
        "format = %lld %lld %lld\n"
        "test = 1 2 3 => 6\n"
        "test = -5 5 10 => 10\n"
        "must = add\n"
        "hint = Сложите a, b и c в rdx.\n"
        "max-instructions = 100\n"
        "[task bad name]\n"
        "oops\n";
    std::vector<std::string> errors;
    auto tasks = Exercises::ParseTasks(file, errors);
    CHECK_EQ(tasks.size(), 1u);
    CHECK_EQ(errors.size(), 2u);  // the bad header and the line without "="
    if (tasks.empty()) return;
    const Exercises::Exercise& t = tasks[0];
    CHECK(t.id == "sum3" && t.tests.size() == 2 && t.mustUse.size() == 1 && t.maxInstructions == 100);
    CHECK_EQ(Narrow(Exercises::Title(t, true)), std::string("Sum of three numbers"));
    CHECK_EQ(Exercises::IdFromSource(t.starter), std::string("sum3"));
    std::string source = Replace(t.starter, "    ; TODO: вычислите ответ в rdx\r\n    xor     edx, edx\r\n",
                                 Code("mov rdx, [a]\nadd rdx, [b]\nadd rdx, [c]"));
    CHECK(Exercises::CheckRequirements(t, source).empty());
    auto exe = BuildProgram("sum3", source);
    if (!exe) return;
    for (const Exercises::Test& test : t.tests) {
        std::string out;
        CHECK(RunExe(*exe, test.input, out) && Exercises::OutputMatches(out, test.expected));
    }
    auto count = Debugger::CountInstructions(*exe, t.tests.back().input, 30000);
    CHECK(count && *count <= t.maxInstructions);
}

TEST(LessonsAndResults) {
    CHECK(Lessons::All().size() >= 9u);
    for (const Lessons::Lesson& l : Lessons::All()) {
        CHECK(Exercises::Find(l.exercise) != nullptr);
        for (bool english : {false, true}) {
            std::string text = Lessons::FileText(l, english);
            CHECK_EQ(Exercises::IdFromSource(text), l.exercise);
            CHECK(text.find("global main") != std::string::npos);
        }
    }
    std::map<std::string, Results::Entry> r;
    Results::Record(r, "sum", 1, 3, "2026-10-31 10:00");
    Results::Record(r, "sum", 3, 3, "2026-10-31 10:05");
    Results::Record(r, "sum", 2, 3, "2026-10-31 10:09");
    CHECK(r["sum"].solved);
    CHECK_EQ(r["sum"].attempts, 3);
    CHECK_EQ(r["sum"].bestPassed, 3);
    CHECK_EQ(r["sum"].firstSolved, std::string("2026-10-31 10:05"));
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path file = fs::path(tmp) / L"asmeditor-tests" / L"results.ini";
    CHECK(Results::Save(file, r, "Иванов И."));
    std::string student;
    auto loaded = Results::Load(file, &student);
    CHECK_EQ(student, std::string("Иванов И."));
    CHECK(loaded["sum"].solved && loaded["sum"].attempts == 3 && loaded["sum"].last == "2026-10-31 10:09");
    std::string csv = Results::ExportCsv(loaded, student, false);
    CHECK(csv.find("Иванов И.;sum;") != std::string::npos);
    CHECK(csv.find(";да;3/3;3;2026-10-31 10:05;2026-10-31 10:09") != std::string::npos);
    CHECK(csv.find(";gcd;") != std::string::npos);  // unattempted tasks are listed too
    // every task has an English text; the English starter's header has no Russian
    for (const Exercises::Exercise& e : Exercises::All()) {
        std::string text = Exercises::StarterText(e, true);
        std::string header = text.substr(0, text.find("default rel"));
        CHECK(!e.descriptionEn.empty() && header.find('\xD0') == std::string::npos && header.find('\xD1') == std::string::npos);
        CHECK_EQ(Exercises::IdFromSource(text), e.id);
        std::istringstream lines(text);
        for (std::string l; std::getline(lines, l);) {
            if (l.find('\xD0') != std::string::npos || l.find('\xD1') != std::string::npos) {
                printf("    RUSSIAN [%s]\n", l.c_str());
                CHECK(false);
            }
        }
    }
    // the teacher's summary of two students
    std::map<std::string, Results::Entry> other;
    Results::Record(other, "max", 2, 4, "2026-10-31 11:00");
    std::string summary = Results::GroupSummary({csv, Results::ExportCsv(other, "Петров П.", false)}, false);
    CHECK(summary.find("Ученик;sum;max;") != std::string::npos);
    CHECK(summary.find("Иванов И.;+;;") != std::string::npos);
    CHECK(summary.find("Петров П.;;2/4;") != std::string::npos);
    CHECK(summary.find(";1\r\n") != std::string::npos && summary.find(";0\r\n") != std::string::npos);
    // requirements: gcd has to be recursive, power may not divide
    const Exercises::Exercise* gcd = Exercises::Find("gcd");
    CHECK(gcd && Exercises::CheckRequirements(*gcd, gcd->starter).size() == 2);  // no div, no recursion yet
    const Exercises::Exercise* power = Exercises::Find("power");
    CHECK(power && Exercises::CheckRequirements(*power, power->starter + "    and rax, rbx\n    div rcx\n").size() == 1);
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
