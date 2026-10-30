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
    for (const Conditions::Condition& c : Conditions::All(0)) {
        if (Tr(c.meaning) == c.meaning) {
            missing++;
            printf("    untranslated condition: %s\n", c.meaning.c_str());
        }
    }
    SetLanguage(L"ru");
    CHECK_EQ(missing, 0);
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
    CHECK(d.Start(launch, host.pty, {}, false, std::nullopt, error));
    CHECK(host.Wait(20000));
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
