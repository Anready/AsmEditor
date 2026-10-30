#include "asmtools.h"

#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace {

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool IsWordChar(unsigned char c) {
    return isalnum(c) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' || c == '#' || c == '~' || c >= 0x80;
}

std::string RTrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string LTrim(const std::string& s) {
    size_t i = s.find_first_not_of(" \t");
    return i == std::string::npos ? std::string() : s.substr(i);
}

// Display width in characters (UTF-8 continuation bytes do not count; a tab counts as reaching the next 4).
int Width(const std::string& s) {
    int w = 0;
    for (unsigned char c : s) {
        if (c == '\t') w = (w / 4 + 1) * 4;
        else if ((c & 0xC0) != 0x80) w++;
    }
    return w;
}

void PadTo(std::string& s, int column) {
    int w = Width(s);
    if (w < column) s.append(column - w, ' ');
    else if (!s.empty() && s.back() != ' ') s += ' ';
}

// Splits a line into code and trailing comment (the tokenizer knows about ';' inside strings).
void SplitComment(const std::string& line, std::string& code, std::string& comment) {
    std::vector<Token> tokens;
    Tokenize(line.data(), static_cast<int>(line.size()), tokens);
    size_t at = std::string::npos;
    for (const Token& t : tokens) {
        if (t.type == T_COMMENT) at = t.start;
    }
    code = RTrim(at == std::string::npos ? line : line.substr(0, at));
    comment = at == std::string::npos ? std::string() : RTrim(line.substr(at));
}

// Operands: single spaces, ", " after commas; strings are kept as they are.
std::string NormalizeOperands(const std::string& s) {
    std::string out;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            out += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            out += c;
        } else if (c == ' ' || c == '\t') {
            if (!out.empty() && out.back() != ' ' && out.back() != '[' && out.back() != '(') out += ' ';
        } else if (c == ',') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += ", ";
            while (i + 1 < s.size() && (s[i + 1] == ' ' || s[i + 1] == '\t')) i++;
        } else if ((c == ']' || c == ')') && !out.empty() && out.back() == ' ') {
            out.back() = c;
        } else {
            out += c;
        }
    }
    return RTrim(out);
}

std::string ReadWord(const std::string& s, size_t from) {
    size_t i = from;
    while (i < s.size() && IsWordChar(static_cast<unsigned char>(s[i]))) i++;
    return s.substr(from, i - from);
}

const std::set<std::string> kTopLevel = {"section", "segment", "global", "extern", "default", "bits", "cpu",
                                         "org", "absolute", "common", "static", "struc", "endstruc", "istruc",
                                         "iend", "use16", "use32", "use64", "import", "export"};
const std::set<std::string> kData = {"db", "dw", "dd", "dq", "dt", "do", "dy", "dz", "resb", "resw", "resd",
                                     "resq", "rest", "reso", "resy", "resz", "equ", "times", "incbin"};
const std::set<std::string> kPrefixes = {"rep", "repe", "repz", "repne", "repnz", "lock", "xacquire", "xrelease",
                                         "bnd", "o16", "o32", "o64", "a16", "a32", "a64"};
}

// ---- formatter --------------------------------------------------------------------------------------------------

namespace AsmFormat {

std::string FormatLine(const std::string& rawLine, const Options& o) {
    std::string line = RTrim(rawLine);
    if (line.empty()) return {};
    std::string code, comment;
    SplitComment(line, code, comment);
    size_t f = code.find_first_not_of(" \t");
    if (f == std::string::npos) return line;  // a comment line keeps its indentation

    auto statement = [&](std::string& out, const std::string& text) {
        size_t m = text.find_first_not_of(" \t");
        std::string mnemonic = ReadWord(text, m);
        size_t rest = m + mnemonic.size();
        if (mnemonic.empty()) {  // something unusual: keep it
            out += LTrim(text);
            return;
        }
        if (kPrefixes.count(LowerAscii(mnemonic))) {  // "rep movsb" counts as one mnemonic
            size_t n = text.find_first_not_of(" \t", rest);
            std::string next = n == std::string::npos ? std::string() : ReadWord(text, n);
            if (!next.empty()) {
                mnemonic += " " + next;
                rest = n + next.size();
            }
        }
        out += mnemonic;
        std::string operands = rest < text.size() ? NormalizeOperands(LTrim(text.substr(rest))) : std::string();
        if (!operands.empty()) {
            PadTo(out, o.operands);
            out += operands;
        }
    };

    std::string out;
    std::vector<Token> tokens;
    Tokenize(code.data(), static_cast<int>(code.size()), tokens);
    const Token* first = nullptr;
    for (const Token& t : tokens) {
        if (t.start == static_cast<int>(f)) first = &t;
    }
    if (code[f] == '%') {
        out = code.substr(f);  // preprocessor: left edge, content untouched
    } else if (first && first->type == T_LABEL && code.find(':', f) < static_cast<size_t>(first->end)) {
        std::string label = code.substr(f, first->end - f);
        label.erase(std::remove_if(label.begin(), label.end(), [](char c) { return c == ' ' || c == '\t'; }), label.end());
        out = label;
        std::string rest = LTrim(code.substr(first->end));
        if (!rest.empty()) {
            PadTo(out, o.indent);
            statement(out, rest);
        }
    } else {
        std::string word = ReadWord(code, f);
        std::string lw = LowerAscii(word);
        size_t after = code.find_first_not_of(" \t", f + word.size());
        std::string next = after == std::string::npos ? std::string() : LowerAscii(ReadWord(code, after));
        if (kTopLevel.count(lw)) {
            out = word;
            std::string rest = after == std::string::npos ? std::string() : NormalizeOperands(code.substr(after));
            if (!rest.empty()) out += " " + rest;
        } else if (!word.empty() && !kData.count(lw) && kData.count(next)) {  // "msg db ...", "SIZE equ 10"
            out = std::string(o.indent, ' ') + word;
            PadTo(out, o.operands);
            std::string directive = ReadWord(code, after);
            out += directive;
            std::string operands = NormalizeOperands(LTrim(code.substr(after + directive.size())));
            if (!operands.empty()) out += " " + operands;
        } else {
            out = std::string(o.indent, ' ');
            statement(out, code.substr(f));
        }
    }
    if (!comment.empty()) {
        PadTo(out, o.comments);
        out += comment;
    }
    return out;
}

std::string Format(const std::string& text, const Options& o, const std::string& eol) {
    std::string out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out += FormatLine(line, o);
        if (last) break;
        out += eol;
        pos = end + 1;
    }
    return out;
}

}

// ---- calling convention check -----------------------------------------------------------------------------------

namespace StackCheck {
namespace {

// A small constant expression: numbers (decimal, 0x, h suffix) with + - *.
std::optional<int64_t> Constant(const std::string& s) {
    std::string t;
    for (char c : s) {
        if (c != ' ' && c != '\t') t += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (t.empty()) return std::nullopt;
    size_t i = 0;
    std::function<std::optional<int64_t>()> term = [&]() -> std::optional<int64_t> {
        auto number = [&]() -> std::optional<int64_t> {
            bool neg = false;
            if (i < t.size() && t[i] == '-') {
                neg = true;
                i++;
            }
            size_t start = i;
            while (i < t.size() && (isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_')) i++;
            std::string n = t.substr(start, i - start);
            n.erase(std::remove(n.begin(), n.end(), '_'), n.end());
            if (n.empty() || !isdigit(static_cast<unsigned char>(n[0]))) return std::nullopt;
            int base = 10;
            if (n.rfind("0x", 0) == 0) {
                base = 16;
                n = n.substr(2);
            } else if (n.back() == 'h') {
                base = 16;
                n.pop_back();
            }
            char* end = nullptr;
            int64_t v = static_cast<int64_t>(strtoull(n.c_str(), &end, base));
            if (n.empty() || *end) return std::nullopt;
            return neg ? -v : v;
        };
        auto v = number();
        while (v && i < t.size() && t[i] == '*') {
            i++;
            auto r = number();
            if (!r) return std::nullopt;
            *v *= *r;
        }
        return v;
    };
    auto v = term();
    while (v && i < t.size() && (t[i] == '+' || t[i] == '-')) {
        bool plus = t[i] == '+';
        i++;
        auto r = term();
        if (!r) return std::nullopt;
        *v = plus ? *v + *r : *v - *r;
    }
    if (!v || i != t.size()) return std::nullopt;
    return v;
}

std::vector<std::string> SplitOperands(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') quote = c;
        if (c == '[' || c == '(') depth++;
        if (c == ']' || c == ')') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(LTrim(RTrim(cur)));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!LTrim(cur).empty()) out.push_back(LTrim(RTrim(cur)));
    return out;
}
}

std::vector<Diagnostic> Analyze(const std::string& text) {
    std::vector<Diagnostic> out;
    std::set<std::string> externs;
    // First pass: external functions (only calls to them are checked) and the bitness.
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::string code, comment;
            SplitComment(line, code, comment);
            std::string t = LTrim(code);
            std::string w = LowerAscii(ReadWord(t, 0));
            if (w == "bits" && t.find("64") == std::string::npos) return out;  // 16/32-bit code: other rules
            if (w == "extern") {
                for (const std::string& name : SplitOperands(t.substr(w.size()))) {
                    std::string n = ReadWord(LTrim(name), 0);
                    if (!n.empty()) externs.insert(n);
                }
            }
        }
    }

    bool known = false;       // rsp is followed inside the current function
    int rspMod = 8;           // rsp modulo 16 (8 at a function's entry: the return address was pushed)
    int rbpMod = -1;          // rsp modulo 16 when "mov rbp, rsp" ran (-1: unknown)
    int64_t allocated = 0;    // bytes reserved with sub rsp (not counting push)
    bool inMacro = false;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    auto mod16 = [](int64_t v) { return static_cast<int>(((v % 16) + 16) % 16); };
    while (std::getline(in, line)) {
        lineNo++;
        std::string code, comment;
        SplitComment(line, code, comment);
        std::string t = LTrim(code);
        if (t.empty()) continue;
        std::string lw = LowerAscii(ReadWord(t, t[0] == '%' ? 1 : 0));
        if (t[0] == '%') {
            if (lw == "macro" || lw == "imacro") inMacro = true;
            if (lw == "endmacro") inMacro = false;
            continue;
        }
        if (inMacro) continue;

        // Labels: a global one starts a function, a local one keeps the state.
        std::vector<Token> tokens;
        Tokenize(code.data(), static_cast<int>(code.size()), tokens);
        if (!tokens.empty() && tokens[0].type == T_LABEL && code.find(':', tokens[0].start) < static_cast<size_t>(tokens[0].end)) {
            std::string label = ReadWord(code, tokens[0].start);
            if (!label.empty() && label[0] != '.') {
                known = true;
                rspMod = 8;
                rbpMod = -1;
                allocated = 0;
            }
            t = LTrim(code.substr(tokens[0].end));
            if (t.empty()) continue;
            lw = LowerAscii(ReadWord(t, 0));
        }
        if (kPrefixes.count(lw)) continue;
        std::string operandText = t.size() > lw.size() ? t.substr(lw.size()) : std::string();
        std::vector<std::string> ops = SplitOperands(operandText);
        auto lowerOp = [&](size_t i) { return i < ops.size() ? LowerAscii(ops[i]) : std::string(); };
        std::string dst = lowerOp(0);
        bool dstRsp = dst == "rsp" || dst == "esp" || dst == "sp";

        if (lw == "push" || lw == "pushfq" || lw == "pushf") {
            rspMod = mod16(rspMod - 8);
        } else if (lw == "pop" || lw == "popfq" || lw == "popf") {
            if (dstRsp) known = false;
            rspMod = mod16(rspMod + 8);
            if (dst == "rbp") rbpMod = -1;
        } else if ((lw == "sub" || lw == "add") && dstRsp) {
            auto n = Constant(ops.size() > 1 ? ops[1] : "");
            if (!n) {
                known = false;
                continue;
            }
            rspMod = mod16(lw == "sub" ? rspMod - *n : rspMod + *n);
            allocated += lw == "sub" ? *n : -*n;
        } else if (lw == "and" && dstRsp) {
            auto n = Constant(ops.size() > 1 ? ops[1] : "");
            if (n && (*n & 0xF) == 0) rspMod = 0;
            else known = false;
        } else if (lw == "mov" && dst == "rbp" && lowerOp(1) == "rsp") {
            rbpMod = rspMod;
        } else if (lw == "mov" && dstRsp && lowerOp(1) == "rbp" && rbpMod >= 0) {
            rspMod = rbpMod;
        } else if (lw == "leave") {
            if (rbpMod < 0) known = false;
            rspMod = mod16(rbpMod + 8);
            rbpMod = -1;
        } else if (lw == "enter") {
            auto n = Constant(ops.empty() ? "" : ops[0]);
            if (!n) known = false;
            else {
                rspMod = mod16(rspMod - 8);
                rbpMod = rspMod;
                rspMod = mod16(rspMod - *n);
                allocated += *n;
            }
        } else if (lw == "ret" || lw == "retn" || lw == "jmp") {
            known = false;  // what follows is reached from elsewhere
        } else if (lw == "call") {
            std::string target = ops.empty() ? std::string() : ops[0];
            std::string ltarget = LowerAscii(target);
            std::string name = ReadWord(LTrim(target), 0);
            bool external = externs.count(name) > 0 || ltarget.find("__imp_") != std::string::npos;
            if (known && external) {
                if (rspMod != 0) {
                    out.push_back({{}, lineNo, Severity::Warning,
                                   Widen(Tr(std::string("Перед call стек, вероятно, не выровнен на 16 байт (rsp mod 16 = 8): "
                                                        "функции C (printf и др.) могут упасть. Измените sub rsp на 8 байт "
                                                        "(например, 40 вместо 32) или уберите лишний push.")))});
                } else if (allocated < 32) {
                    out.push_back({{}, lineNo, Severity::Warning,
                                   Widen(Tr(std::string("Перед вызовом внешней функции не выделено теневое место: "
                                                        "по соглашению Win64 нужно sub rsp, 32 (или больше) — "
                                                        "вызываемая функция может затереть данные в стеке.")))});
                }
            }
        } else if (dstRsp && lw != "cmp" && lw != "test") {
            known = false;  // some other change of rsp
        }
    }
    return out;
}

}

// ---- disassembler -----------------------------------------------------------------------------------------------

namespace Disasm {

std::vector<Line> Parse(const std::string& output) {
    std::vector<Line> lines;
    static const std::regex row("^([0-9A-Fa-f]+)\\s+([0-9A-Fa-f]+)\\s+(.*)$");
    static const std::regex more("^\\s+-([0-9A-Fa-f]+)\\s*$");
    std::istringstream in(output);
    std::string l;
    while (std::getline(in, l)) {
        l = RTrim(l);
        std::smatch m;
        if (std::regex_match(l, m, more) && !lines.empty()) {
            lines.back().bytes += m[1].str();  // long instruction continued on the next line
        } else if (std::regex_match(l, m, row)) {
            Line d;
            d.address = strtoull(m[1].str().c_str(), nullptr, 16);
            d.bytes = m[2].str();
            d.text = m[3].str();
            lines.push_back(std::move(d));
        }
    }
    return lines;
}

std::vector<Line> Run(const fs::path& ndisasm, const uint8_t* data, size_t size, uint64_t address, int bits) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path file = fs::path(tmp) / (L"asmeditor-disasm-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    char origin[32];
    sprintf_s(origin, "0x%llX", static_cast<unsigned long long>(address));
    std::string output;
    Toolchain::Exec({ndisasm.wstring(), L"-b", std::to_wstring(bits), L"-o", Widen(origin), file.wstring()}, {},
                    nullptr, &output);
    std::error_code ec;
    fs::remove(file, ec);
    return Parse(output);
}

}

// ---- snippets ---------------------------------------------------------------------------------------------------

namespace Snippets {

const std::vector<Snippet>& All() {
    static const std::vector<Snippet> list = {
        {"func", "Функция Win64 с прологом и эпилогом",
         "my_function:\n"
         "    push    rbp\n"
         "    mov     rbp, rsp\n"
         "    sub     rsp, 32                     ; теневое место для вызовов (стек выровнен на 16)\n"
         "    |\n"
         "    mov     rsp, rbp\n"
         "    pop     rbp\n"
         "    ret\n"},
        {"loop", "Цикл со счётчиком",
         "    mov     ebx, 10                     ; число повторений (rbx сохраняется при вызовах)\n"
         ".loop:\n"
         "    |\n"
         "    dec     ebx\n"
         "    jnz     .loop\n"},
        {"if", "Сравнение и ветвление",
         "    cmp     rax, rbx\n"
         "    jne     .else\n"
         "    |                                   ; rax == rbx\n"
         "    jmp     .end_if\n"
         ".else:\n"
         "                                        ; rax != rbx\n"
         ".end_if:\n"},
        {"printf", "Вызов printf (нужны extern printf и строка формата)",
         "    lea     rcx, [fmt]                  ; 1-й аргумент: строка формата, например fmt db \"%lld\", 10, 0\n"
         "    mov     rdx, rax                    ; 2-й аргумент: число\n"
         "    call    printf|\n"},
        {"scanf", "Ввод числа через scanf (нужны extern scanf, fmt_in db \"%lld\", 0 и number dq 0)",
         "    lea     rcx, [fmt_in]               ; строка формата \"%lld\"\n"
         "    lea     rdx, [number]               ; адрес, куда записать число\n"
         "    call    scanf\n"
         "    mov     rax, [number]|\n"},
        {"winapi", "Вывод строки через WriteFile (без C runtime)",
         "    mov     ecx, -11                    ; STD_OUTPUT_HANDLE\n"
         "    call    GetStdHandle\n"
         "    mov     rcx, rax                    ; дескриптор консоли\n"
         "    lea     rdx, [msg]                  ; текст\n"
         "    mov     r8d, msg_len                ; длина\n"
         "    lea     r9, [written]               ; сюда запишется число байт\n"
         "    mov     qword [rsp + 32], 0         ; 5-й аргумент — в стеке, над теневым местом\n"
         "    call    WriteFile|\n"},
        {"main", "Шаблон программы (C runtime, printf)",
         "default rel\n"
         "\n"
         "extern printf\n"
         "\n"
         "global main\n"
         "\n"
         "section .data\n"
         "    msg     db \"Hello, world!\", 10, 0\n"
         "\n"
         "section .text\n"
         "main:\n"
         "    sub     rsp, 40                     ; теневое место (32 байта) + выравнивание стека на 16\n"
         "    lea     rcx, [msg]\n"
         "    call    printf\n"
         "    |\n"
         "    add     rsp, 40\n"
         "    xor     eax, eax                    ; return 0\n"
         "    ret\n"},
        {"data", "Секция данных",
         "section .data\n"
         "    msg     db \"Текст\", 10, 0          ; строка с переводом строки и нулём в конце\n"
         "    number  dq 0                        ; 8 байт\n"
         "|\n"
         "section .bss\n"
         "    buffer  resb 256                    ; 256 байт без начального значения\n"},
    };
    return list;
}

std::string Expand(const Snippet& s, const std::string& eol, size_t& caret) {
    std::string out;
    caret = std::string::npos;
    for (const char* p = s.body; *p; p++) {
        if (*p == '|' && caret == std::string::npos) {
            caret = out.size();
        } else if (*p == '\n') {
            out += eol;
        } else {
            out += *p;
        }
    }
    if (caret == std::string::npos) caret = out.size();
    return out;
}

}

// ---- project templates ------------------------------------------------------------------------------------------

namespace Templates {

const std::vector<Template>& All() {
    static const std::vector<Template> list = {
        {L"Hello, world (printf)", L"main.asm",
         "; Hello, world для Windows x64 (NASM + MSVC link + C runtime)\r\n"
         "default rel\r\n"
         "\r\n"
         "extern printf\r\n"
         "\r\n"
         "global main\r\n"
         "\r\n"
         "section .data\r\n"
         "    msg     db \"Hello, world!\", 10, 0\r\n"
         "\r\n"
         "section .text\r\n"
         "main:\r\n"
         "    sub     rsp, 40             ; shadow space (32 байта) + выравнивание стека на 16\r\n"
         "    lea     rcx, [msg]          ; 1-й аргумент — в rcx\r\n"
         "    call    printf\r\n"
         "    add     rsp, 40\r\n"
         "    xor     eax, eax            ; return 0\r\n"
         "    ret\r\n"},
        {L"Ввод и вывод: scanf / printf", L"main.asm",
         "; Читает два числа и печатает их сумму (scanf и printf из C runtime)\r\n"
         "default rel\r\n"
         "\r\n"
         "extern printf\r\n"
         "extern scanf\r\n"
         "\r\n"
         "global main\r\n"
         "\r\n"
         "section .data\r\n"
         "    prompt  db \"Введите два числа: \", 0\r\n"
         "    fmt_in  db \"%lld %lld\", 0\r\n"
         "    fmt_out db \"Сумма: %lld\", 10, 0\r\n"
         "\r\n"
         "section .bss\r\n"
         "    a       resq 1\r\n"
         "    b       resq 1\r\n"
         "\r\n"
         "section .text\r\n"
         "main:\r\n"
         "    sub     rsp, 40             ; теневое место + выравнивание\r\n"
         "\r\n"
         "    lea     rcx, [prompt]\r\n"
         "    call    printf\r\n"
         "\r\n"
         "    lea     rcx, [fmt_in]       ; scanf(\"%lld %lld\", &a, &b)\r\n"
         "    lea     rdx, [a]\r\n"
         "    lea     r8, [b]\r\n"
         "    call    scanf\r\n"
         "\r\n"
         "    mov     rdx, [a]\r\n"
         "    add     rdx, [b]            ; 2-й аргумент printf — сумма\r\n"
         "    lea     rcx, [fmt_out]\r\n"
         "    call    printf\r\n"
         "\r\n"
         "    add     rsp, 40\r\n"
         "    xor     eax, eax\r\n"
         "    ret\r\n"},
        {L"Чистый WinAPI (без printf)", L"main.asm",
         "; Вывод в консоль только функциями Windows: GetStdHandle, WriteFile, ExitProcess\r\n"
         "default rel\r\n"
         "\r\n"
         "extern GetStdHandle\r\n"
         "extern WriteFile\r\n"
         "extern ExitProcess\r\n"
         "\r\n"
         "global main\r\n"
         "\r\n"
         "section .data\r\n"
         "    msg     db \"Hello from WinAPI!\", 13, 10\r\n"
         "    msg_len equ $ - msg\r\n"
         "\r\n"
         "section .bss\r\n"
         "    written resd 1\r\n"
         "\r\n"
         "section .text\r\n"
         "main:\r\n"
         "    sub     rsp, 56             ; теневое место 32 + 5-й аргумент + выравнивание\r\n"
         "\r\n"
         "    mov     ecx, -11            ; STD_OUTPUT_HANDLE\r\n"
         "    call    GetStdHandle\r\n"
         "\r\n"
         "    mov     rcx, rax            ; hFile\r\n"
         "    lea     rdx, [msg]          ; lpBuffer\r\n"
         "    mov     r8d, msg_len        ; nNumberOfBytesToWrite\r\n"
         "    lea     r9, [written]       ; lpNumberOfBytesWritten\r\n"
         "    mov     qword [rsp + 32], 0 ; lpOverlapped = NULL (5-й аргумент — в стеке)\r\n"
         "    call    WriteFile\r\n"
         "\r\n"
         "    xor     ecx, ecx            ; код возврата 0\r\n"
         "    call    ExitProcess\r\n"},
        {L"Работа с файлами (fopen / fprintf / fgets)", L"main.asm",
         "; Записывает строку в файл, потом читает её и печатает\r\n"
         "default rel\r\n"
         "\r\n"
         "extern fopen\r\n"
         "extern fclose\r\n"
         "extern fprintf\r\n"
         "extern fgets\r\n"
         "extern printf\r\n"
         "\r\n"
         "global main\r\n"
         "\r\n"
         "section .data\r\n"
         "    name    db \"out.txt\", 0\r\n"
         "    mode_w  db \"w\", 0\r\n"
         "    mode_r  db \"r\", 0\r\n"
         "    text    db \"Строка из программы на ассемблере\", 10, 0\r\n"
         "    fmt     db \"Прочитано: %s\", 0\r\n"
         "    err     db \"Не удалось открыть файл\", 10, 0\r\n"
         "\r\n"
         "section .bss\r\n"
         "    buffer  resb 256\r\n"
         "\r\n"
         "section .text\r\n"
         "main:\r\n"
         "    push    rbx                 ; rbx сохраняем: будем хранить в нём FILE*\r\n"
         "    sub     rsp, 32             ; теневое место (push + 32 = выравнивание на 16)\r\n"
         "\r\n"
         "    lea     rcx, [name]         ; fopen(\"out.txt\", \"w\")\r\n"
         "    lea     rdx, [mode_w]\r\n"
         "    call    fopen\r\n"
         "    test    rax, rax\r\n"
         "    jz      .fail\r\n"
         "    mov     rbx, rax\r\n"
         "\r\n"
         "    mov     rcx, rbx            ; fprintf(f, text)\r\n"
         "    lea     rdx, [text]\r\n"
         "    call    fprintf\r\n"
         "    mov     rcx, rbx\r\n"
         "    call    fclose\r\n"
         "\r\n"
         "    lea     rcx, [name]         ; fopen(\"out.txt\", \"r\")\r\n"
         "    lea     rdx, [mode_r]\r\n"
         "    call    fopen\r\n"
         "    test    rax, rax\r\n"
         "    jz      .fail\r\n"
         "    mov     rbx, rax\r\n"
         "\r\n"
         "    lea     rcx, [buffer]       ; fgets(buffer, 256, f)\r\n"
         "    mov     edx, 256\r\n"
         "    mov     r8, rbx\r\n"
         "    call    fgets\r\n"
         "    mov     rcx, rbx\r\n"
         "    call    fclose\r\n"
         "\r\n"
         "    lea     rcx, [fmt]\r\n"
         "    lea     rdx, [buffer]\r\n"
         "    call    printf\r\n"
         "    xor     eax, eax\r\n"
         "    jmp     .done\r\n"
         ".fail:\r\n"
         "    lea     rcx, [err]\r\n"
         "    call    printf\r\n"
         "    mov     eax, 1\r\n"
         ".done:\r\n"
         "    add     rsp, 32\r\n"
         "    pop     rbx\r\n"
         "    ret\r\n"},
        {L"Числа с плавающей точкой (SSE)", L"main.asm",
         "; Складывает два массива float по 4 числа за раз (SSE) и печатает сумму двух double\r\n"
         "default rel\r\n"
         "\r\n"
         "extern printf\r\n"
         "\r\n"
         "global main\r\n"
         "\r\n"
         "section .data\r\n"
         "    align 16\r\n"
         "    a       dd 1.0, 2.0, 3.0, 4.0\r\n"
         "    b       dd 10.0, 20.0, 30.0, 40.0\r\n"
         "    x       dq 1.5\r\n"
         "    y       dq 2.25\r\n"
         "    fmt4    db \"a + b = %g %g %g %g\", 10, 0\r\n"
         "    fmt1    db \"x + y = %g\", 10, 0\r\n"
         "\r\n"
         "section .bss\r\n"
         "    alignb 16\r\n"
         "    c       resd 4\r\n"
         "\r\n"
         "section .text\r\n"
         "main:\r\n"
         "    sub     rsp, 56             ; теневое место + место для 5-го аргумента + выравнивание\r\n"
         "\r\n"
         "    movaps  xmm0, [a]           ; четыре float из a (адрес выровнен на 16)\r\n"
         "    addps   xmm0, [b]           ; + четыре float из b\r\n"
         "    movaps  [c], xmm0\r\n"
         "\r\n"
         "    ; printf получает float как double; аргументы с плавающей точкой varargs-функции\r\n"
         "    ; передаются и в xmm, и в обычных регистрах\r\n"
         "    cvtss2sd xmm1, [c]\r\n"
         "    movq    rdx, xmm1\r\n"
         "    cvtss2sd xmm2, [c + 4]\r\n"
         "    movq    r8, xmm2\r\n"
         "    cvtss2sd xmm3, [c + 8]\r\n"
         "    movq    r9, xmm3\r\n"
         "    cvtss2sd xmm4, [c + 12]\r\n"
         "    movsd   [rsp + 32], xmm4    ; 5-й аргумент — в стеке\r\n"
         "    lea     rcx, [fmt4]\r\n"
         "    call    printf\r\n"
         "\r\n"
         "    movsd   xmm1, [x]\r\n"
         "    addsd   xmm1, [y]           ; скалярное сложение double\r\n"
         "    movq    rdx, xmm1\r\n"
         "    lea     rcx, [fmt1]\r\n"
         "    call    printf\r\n"
         "\r\n"
         "    add     rsp, 56\r\n"
         "    xor     eax, eax\r\n"
         "    ret\r\n"},
    };
    return list;
}

}

// ---- exercises --------------------------------------------------------------------------------------------------

namespace Exercises {
namespace {

std::string Starter(const std::string& id, const std::string& description, const std::string& inFormat,
                    const std::string& body) {
    std::string text = "; task: " + id + "\r\n";
    std::istringstream in(description);
    std::string line;
    while (std::getline(in, line)) text += "; " + line + "\r\n";
    text += ";\r\n; Проверка решения: Запуск → «Проверить решение» (Ctrl+Shift+T).\r\n"
            "default rel\r\n"
            "\r\n"
            "extern printf\r\n"
            "extern scanf\r\n"
            "\r\n"
            "global main\r\n"
            "\r\n"
            "section .data\r\n"
            "    fmt_in  db \"" + inFormat + "\", 0\r\n"
            "    fmt_out db \"%lld\", 10, 0\r\n"
            "\r\n" + body;
    return text;
}

const char* kReadOneNumber =
    "section .bss\r\n"
    "    n       resq 1\r\n"
    "\r\n"
    "section .text\r\n"
    "main:\r\n"
    "    sub     rsp, 40\r\n"
    "    lea     rcx, [fmt_in]\r\n"
    "    lea     rdx, [n]\r\n"
    "    call    scanf\r\n"
    "\r\n"
    "    ; TODO: вычислите ответ в rdx\r\n"
    "    mov     rdx, [n]\r\n"
    "\r\n"
    "    lea     rcx, [fmt_out]\r\n"
    "    call    printf\r\n"
    "    add     rsp, 40\r\n"
    "    xor     eax, eax\r\n"
    "    ret\r\n";
}

const std::vector<Exercise>& All() {
    static const std::vector<Exercise> list = [] {
        std::vector<Exercise> v;
        auto add = [&](std::string id, std::wstring title, std::string description, std::string inFormat,
                       std::string body, std::vector<Test> tests) {
            Exercise e{id, title, description, Starter(id, description, inFormat, body), std::move(tests)};
            v.push_back(std::move(e));
        };
        add("sum", L"1. Сумма двух чисел",
            "Прочитайте два целых числа и напечатайте их сумму.\nПример: ввод «2 3» — вывод «5».", "%lld %lld",
            "section .bss\r\n"
            "    a       resq 1\r\n"
            "    b       resq 1\r\n"
            "\r\n"
            "section .text\r\n"
            "main:\r\n"
            "    sub     rsp, 40\r\n"
            "    lea     rcx, [fmt_in]\r\n"
            "    lea     rdx, [a]\r\n"
            "    lea     r8, [b]\r\n"
            "    call    scanf\r\n"
            "\r\n"
            "    ; TODO: rdx = a + b\r\n"
            "    mov     rdx, [a]\r\n"
            "\r\n"
            "    lea     rcx, [fmt_out]\r\n"
            "    call    printf\r\n"
            "    add     rsp, 40\r\n"
            "    xor     eax, eax\r\n"
            "    ret\r\n",
            {{"2 3", "5"}, {"-4 10", "6"}, {"1000000000000 1", "1000000000001"}});
        add("max", L"2. Максимум из трёх",
            "Прочитайте три целых числа и напечатайте наибольшее.\nПример: ввод «3 9 4» — вывод «9».", "%lld %lld %lld",
            "section .bss\r\n"
            "    a       resq 1\r\n"
            "    b       resq 1\r\n"
            "    c       resq 1\r\n"
            "\r\n"
            "section .text\r\n"
            "main:\r\n"
            "    sub     rsp, 40\r\n"
            "    lea     rcx, [fmt_in]\r\n"
            "    lea     rdx, [a]\r\n"
            "    lea     r8, [b]\r\n"
            "    lea     r9, [c]\r\n"
            "    call    scanf\r\n"
            "\r\n"
            "    ; TODO: rdx = max(a, b, c) — пригодятся cmp и cmovl / jge\r\n"
            "    mov     rdx, [a]\r\n"
            "\r\n"
            "    lea     rcx, [fmt_out]\r\n"
            "    call    printf\r\n"
            "    add     rsp, 40\r\n"
            "    xor     eax, eax\r\n"
            "    ret\r\n",
            {{"3 9 4", "9"}, {"-1 -5 -3", "-1"}, {"7 7 2", "7"}, {"1 2 30", "30"}});
        add("factorial", L"3. Факториал",
            "Прочитайте n (0 ≤ n ≤ 20) и напечатайте n! (произведение 1·2·…·n, 0! = 1).\nПример: ввод «5» — вывод «120».",
            "%lld", kReadOneNumber, {{"5", "120"}, {"0", "1"}, {"1", "1"}, {"20", "2432902008176640000"}});
        add("fib", L"4. Числа Фибоначчи",
            "Прочитайте n (1 ≤ n ≤ 90) и напечатайте n-е число Фибоначчи: F1 = F2 = 1, Fn = Fn-1 + Fn-2.\n"
            "Пример: ввод «10» — вывод «55».",
            "%lld", kReadOneNumber, {{"1", "1"}, {"2", "1"}, {"10", "55"}, {"50", "12586269025"}});
        add("digits", L"5. Сумма цифр",
            "Прочитайте неотрицательное целое число и напечатайте сумму его цифр (деление div на 10).\n"
            "Пример: ввод «1234» — вывод «10».",
            "%lld", kReadOneNumber, {{"1234", "10"}, {"0", "0"}, {"9999999999", "90"}});
        add("power", L"6. Степень двойки",
            "Прочитайте n и напечатайте «yes», если n — степень двойки (1, 2, 4, 8…), иначе «no».\n"
            "Подсказка: у степени двойки n & (n - 1) = 0. Строки выводите своим форматом, например\n"
            "yes db \"yes\", 10, 0 и printf с ним.",
            "%lld", kReadOneNumber, {{"1", "yes"}, {"64", "yes"}, {"12", "no"}, {"0", "no"}, {"1024", "yes"}});
        return v;
    }();
    return list;
}

const Exercise* Find(const std::string& id) {
    for (const Exercise& e : All()) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::string IdFromSource(const std::string& text) {
    static const std::regex re(";\\s*task:\\s*([A-Za-z0-9_-]+)");
    std::smatch m;
    std::string head = text.substr(0, 2000);
    if (std::regex_search(head, m, re)) return m[1].str();
    return {};
}

bool OutputMatches(const std::string& output, const std::string& expected) {
    auto words = [](const std::string& s) {
        std::vector<std::string> w;
        std::istringstream in(s);
        std::string t;
        while (in >> t) w.push_back(t);
        return w;
    };
    std::vector<std::string> out = words(output), exp = words(expected);
    if (exp.empty()) return out.empty();
    for (size_t i = 0; i + exp.size() <= out.size(); i++) {
        if (std::equal(exp.begin(), exp.end(), out.begin() + i)) return true;
    }
    return false;
}

}
