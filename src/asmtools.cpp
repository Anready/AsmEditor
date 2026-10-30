#include "asmtools.h"

#include "hints.h"

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
                                                        "(например, 40 вместо 32) или уберите лишний push."))),
                                   "stack-align"});
                } else if (allocated < 32) {
                    out.push_back({{}, lineNo, Severity::Warning,
                                   Widen(Tr(std::string("Перед вызовом внешней функции не выделено теневое место: "
                                                        "по соглашению Win64 нужно sub rsp, 32 (или больше) — "
                                                        "вызываемая функция может затереть данные в стеке."))),
                                   "shadow"});
                }
            }
        } else if (dstRsp && lw != "cmp" && lw != "test") {
            known = false;  // some other change of rsp
        }
    }
    return out;
}

}

// ---- analyzer ---------------------------------------------------------------------------------------------------

namespace Analyzer {
namespace {

// One statement of the source: label, mnemonic and operands.
struct Stmt {
    int line = 0;                     // 0-based
    std::string label;                // label defined on the line
    bool function = false;            // a global label with ':' (starts a function)
    std::string mnemonic;             // lower case ("" for a line with only a label)
    std::vector<std::string> ops;     // lower case
    std::vector<std::string> rawOps;  // as written (names are case-sensitive)
    std::string indent;               // whitespace before the mnemonic
};

std::vector<Stmt> Parse(const std::string& text) {
    std::vector<Stmt> out;
    std::istringstream in(text);
    std::string raw;
    bool inMacro = false, codeSection = true;
    for (int no = 0; std::getline(in, raw); no++) {
        std::string code, comment;
        SplitComment(RTrim(raw), code, comment);
        std::string t = LTrim(code);
        if (t.empty()) continue;
        if (t[0] == '%') {
            std::string w = LowerAscii(ReadWord(t, 1));
            if (w == "macro" || w == "imacro") inMacro = true;
            else if (w == "endmacro") inMacro = false;
            continue;
        }
        if (inMacro) continue;
        Stmt st;
        st.line = no;
        std::string rest = code;
        std::vector<Token> tokens;
        Tokenize(code.data(), static_cast<int>(code.size()), tokens);
        if (!tokens.empty() && tokens[0].type == T_LABEL) {
            st.label = ReadWord(code, tokens[0].start);
            bool colon = code.find(':', tokens[0].start) < static_cast<size_t>(tokens[0].end);
            st.function = colon && !st.label.empty() && st.label[0] != '.' && st.label.find("..") != 0;
            rest = code.substr(tokens[0].end);
            size_t c = rest.find_first_not_of(" \t");
            if (c != std::string::npos && rest[c] == ':') rest = rest.substr(c + 1);
        }
        t = LTrim(rest);
        std::string w = LowerAscii(ReadWord(t, 0));
        if (w == "section" || w == "segment") {
            std::string name = LowerAscii(LTrim(t.substr(w.size())));
            codeSection = name.find("text") != std::string::npos || name.find("code") != std::string::npos;
            continue;
        }
        if (!codeSection || kTopLevel.count(w)) continue;
        if (kPrefixes.count(w)) {
            t = LTrim(t.substr(w.size()));
            w = LowerAscii(ReadWord(t, 0));
        }
        st.mnemonic = w;
        size_t at = rest.find_first_not_of(" \t");
        st.indent = st.label.empty() && at != std::string::npos ? rest.substr(0, at) : "    ";
        for (const std::string& op : SplitOperands(t.size() > w.size() ? t.substr(w.size()) : std::string())) {
            st.rawOps.push_back(op);
            st.ops.push_back(LowerAscii(op));
        }
        out.push_back(std::move(st));
    }
    return out;
}

// 64-bit register a register name belongs to ("ecx" -> "rcx"), or "" for anything else.
std::string Canon(const std::string& r) {
    static const std::map<std::string, std::string> names = [] {
        std::map<std::string, std::string> m;
        const char* r64[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp"};
        const char* r32[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"};
        const char* r16[] = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"};
        const char* r8[] = {"al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl"};
        for (int i = 0; i < 8; i++) m[r64[i]] = m[r32[i]] = m[r16[i]] = m[r8[i]] = r64[i];
        m["ah"] = "rax", m["bh"] = "rbx", m["ch"] = "rcx", m["dh"] = "rdx";
        for (int i = 8; i < 16; i++) {
            std::string n = "r" + std::to_string(i);
            m[n] = m[n + "d"] = m[n + "w"] = m[n + "b"] = m[n + "l"] = n;
        }
        return m;
    }();
    auto it = names.find(r);
    return it == names.end() ? std::string() : it->second;
}

bool IsByteRegister(const std::string& r) {
    static const std::set<std::string> bytes = {"al", "bl", "cl", "dl", "ah", "bh", "ch", "dh", "sil", "dil", "bpl", "spl"};
    return bytes.count(r) || (r.size() > 2 && r[0] == 'r' && (r.back() == 'b' || r.back() == 'l') && isdigit(static_cast<unsigned char>(r[1])));
}

// Registers (64-bit names, and xmm0-15) an instruction writes.
std::vector<std::string> Writes(const Stmt& s) {
    const std::string& m = s.mnemonic;
    std::vector<std::string> out;
    static const std::set<std::string> readOnly = {"cmp", "test", "push", "call", "jmp", "ret", "retn", "nop", "int",
                                                   "bt", "ucomisd", "ucomiss", "comisd", "comiss", "ptest", "loop",
                                                   "loope", "loopne", "prefetch", "clflush"};
    if (m.empty() || kData.count(m)) return out;
    if (m == "div" || m == "idiv" || ((m == "mul" || m == "imul") && s.ops.size() == 1)) return {"rax", "rdx"};
    if (m == "cqo" || m == "cdq" || m == "cwd") return {"rdx"};
    if (m == "cpuid") return {"rax", "rbx", "rcx", "rdx"};
    if (m == "rdtsc") return {"rax", "rdx"};
    if (m.rfind("movs", 0) == 0 && s.ops.empty()) return {"rsi", "rdi"};
    if ((m.rfind("lods", 0) == 0 || m.rfind("cmps", 0) == 0) && s.ops.empty()) return {"rsi", "rax"};
    if ((m.rfind("stos", 0) == 0 || m.rfind("scas", 0) == 0) && s.ops.empty()) return {"rdi"};
    if (readOnly.count(m) || (m[0] == 'j' && m != "jmp")) return out;
    if (s.ops.empty()) return out;
    std::string d = Canon(s.ops[0]);
    if (!d.empty()) out.push_back(d);
    else if (s.ops[0].rfind("xmm", 0) == 0) out.push_back(s.ops[0]);
    if (m == "xchg" && s.ops.size() > 1 && !Canon(s.ops[1]).empty()) out.push_back(Canon(s.ops[1]));
    return out;
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// Index ranges [first, last) of the functions (a global label with ':' up to the next one).
std::vector<std::pair<size_t, size_t>> Functions(const std::vector<Stmt>& st) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < st.size(); i++) {
        if (!st[i].function) continue;
        if (!out.empty() && out.back().second == st.size()) out.back().second = i;
        out.push_back({i, st.size()});
    }
    return out;
}

std::pair<size_t, size_t> FunctionAt(const std::vector<Stmt>& st, int line) {
    for (auto f : Functions(st)) {
        int from = st[f.first].line, to = f.second < st.size() ? st[f.second].line : INT_MAX;
        if (line >= from && line < to) return f;
    }
    return {0, 0};
}

std::string Format(const char* russianTemplate, const std::string& arg) {
    std::string t = Tr(std::string(russianTemplate));
    size_t at = t.find("%s");
    return at == std::string::npos ? t : t.replace(at, 2, arg);
}

const char* kCalleeSaved[] = {"rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15"};

// A constant from "sub rsp, 40".
std::optional<int64_t> Number(const std::string& s) {
    std::string t = LowerAscii(LTrim(RTrim(s)));
    if (t.empty() || !isdigit(static_cast<unsigned char>(t[0]))) return std::nullopt;
    int base = 10;
    if (t.rfind("0x", 0) == 0) base = 16, t = t.substr(2);
    else if (t.back() == 'h') base = 16, t.pop_back();
    char* end = nullptr;
    int64_t v = static_cast<int64_t>(strtoull(t.c_str(), &end, base));
    return *end ? std::nullopt : std::optional<int64_t>(v);
}

// "[rsp+40]" (with an optional size before it) -> 40.
std::optional<int64_t> RspSlot(const std::string& op) {
    size_t open = op.find('['), close = op.find(']');
    if (open == std::string::npos || close == std::string::npos) return std::nullopt;
    std::string in;
    for (char c : op.substr(open + 1, close - open - 1)) {
        if (c != ' ' && c != '\t') in += c;
    }
    if (in.rfind("rsp+", 0) != 0) return std::nullopt;
    return Number(in.substr(4));
}
}

std::vector<Diagnostic> Analyze(const std::string& text) {
    std::vector<Diagnostic> out;
    std::vector<Stmt> st = Parse(text);
    std::set<std::string> instructions;
    for (const std::string& i : Hints::KnownInstructions()) instructions.insert(i);
    auto warn = [&](const Stmt& s, const std::string& message, const std::string& code) {
        out.push_back({{}, s.line + 1, Severity::Warning, Widen(message), code});
    };

    for (auto [first, last] : Functions(st)) {
        bool returns = false;
        std::vector<std::string> saved;
        for (size_t i = first; i < last; i++) {
            const Stmt& s = st[i];
            if (s.mnemonic == "ret" || s.mnemonic == "retn") returns = true;
            if (s.mnemonic == "push" && !s.ops.empty() && !Canon(s.ops[0]).empty()) saved.push_back(Canon(s.ops[0]));
            if (s.mnemonic == "mov" && s.ops.size() == 2 && s.ops[0].find('[') != std::string::npos && !Canon(s.ops[1]).empty()) {
                saved.push_back(Canon(s.ops[1]));  // mov [rsp+8], rbx
            }
        }
        // Callee-saved registers: the caller expects them unchanged after the call.
        std::set<std::string> reported;
        for (size_t i = first; i < last && returns; i++) {
            for (const std::string& r : Writes(st[i])) {
                bool calleeSaved = std::find(std::begin(kCalleeSaved), std::end(kCalleeSaved), r) != std::end(kCalleeSaved);
                if (!calleeSaved || Has(saved, r) || reported.count(r)) continue;
                reported.insert(r);
                warn(st[i],
                     Format("%s изменяется, но не сохраняется: по соглашению о вызовах функция должна вернуть его прежним "
                            "(push в начале, pop перед ret) — иначе вызвавший код (в том числе C runtime для main) "
                            "получит испорченное значение.", r),
                     "callee-saved:" + r);
            }
        }
        // div / idiv divide rdx:rax: rdx has to be prepared right before.
        for (size_t i = first; i < last; i++) {
            const Stmt& s = st[i];
            if ((s.mnemonic != "div" && s.mnemonic != "idiv") || s.ops.size() != 1) continue;
            const std::string& op = s.ops[0];
            if (IsByteRegister(op) || op.rfind("byte", 0) == 0) continue;  // 8-bit division uses ax only
            bool prepared = false;
            for (size_t k = i; k-- > first;) {
                const Stmt& p = st[k];
                if (p.mnemonic == "call" || p.mnemonic == "div" || p.mnemonic == "idiv") break;
                if (p.mnemonic != "mul" && p.mnemonic != "imul" && Has(Writes(p), "rdx")) {
                    prepared = true;
                    break;
                }
                if (!p.label.empty()) break;
            }
            if (prepared) continue;
            if (s.mnemonic == "div") {
                warn(s, Tr(std::string("div делит пару rdx:rax: перед ним нужно обнулить rdx (xor edx, edx), иначе частное "
                                       "будет неверным или программа упадёт с «переполнением при делении».")),
                     "div-rdx");
            } else {
                warn(s, Tr(std::string("idiv делит пару rdx:rax: перед ним нужно расширить знак rax в rdx (cqo; для 32 бит — "
                                       "cdq), иначе частное будет неверным или программа упадёт.")),
                     "idiv-rdx");
            }
        }
    }

    // Instructions right after ret / jmp that no label leads to.
    bool dead = false;
    for (const Stmt& s : st) {
        if (!s.label.empty()) dead = false;
        if (dead && !s.mnemonic.empty() && instructions.count(s.mnemonic) && s.mnemonic != "nop" && s.mnemonic != "int3") {
            warn(s, Tr(std::string("Недостижимый код: перед ним ret или jmp, а метки, на которую можно перейти, нет.")),
                 "unreachable");
            dead = false;
        }
        if (s.mnemonic == "ret" || s.mnemonic == "retn" || s.mnemonic == "jmp" || s.mnemonic == "ud2") dead = true;
    }
    std::sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) { return a.line < b.line; });
    return out;
}

std::vector<Fix> QuickFixes(const std::string& text, const Diagnostic& d) {
    std::vector<Fix> fixes;
    std::vector<Stmt> st = Parse(text);
    int line = d.line - 1;
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) lines.push_back(RTrim(l));
    }
    auto indentOf = [&](int l) {
        if (l < 0 || l >= static_cast<int>(lines.size())) return std::string("    ");
        size_t at = lines[l].find_first_not_of(" \t");
        return at == std::string::npos || at == 0 ? std::string("    ") : lines[l].substr(0, at);
    };
    auto [first, last] = FunctionAt(st, line);
    bool inFunction = last > first;
    // Insert after the function's label and before each of its ret.
    auto wrap = [&](const std::string& atStart, const std::string& beforeRet) {
        std::vector<Edit> edits;
        int labelLine = st[first].line;
        int bodyIndentLine = first + 1 < last ? st[first + 1].line : labelLine;
        if (st[first].mnemonic.empty()) edits.push_back({labelLine + 1, false, indentOf(bodyIndentLine) + atStart});
        else edits.push_back({labelLine, false, indentOf(bodyIndentLine) + atStart});  // "f: mov…" on one line
        for (size_t i = first; i < last && !beforeRet.empty(); i++) {
            if (st[i].mnemonic == "ret" || st[i].mnemonic == "retn") {
                edits.push_back({st[i].line, false, indentOf(st[i].line) + beforeRet});
            }
        }
        return edits;
    };

    const std::string& code = d.code;
    if (code.rfind("undefined:", 0) == 0) {
        std::string name = code.substr(10);
        bool called = false;
        for (const Stmt& s : st) {
            if (s.line == line && s.mnemonic == "call") called = true;
        }
        if (called || !Hints::Function(name).empty()) {
            int at = 0, lastExtern = -1, firstSection = -1;
            for (int i = 0; i < static_cast<int>(lines.size()); i++) {
                std::string w = LowerAscii(ReadWord(LTrim(lines[i]), 0));
                if (w == "extern") lastExtern = i;
                if ((w == "section" || w == "segment" || w == "global") && firstSection < 0) firstSection = i;
            }
            at = lastExtern >= 0 ? lastExtern + 1 : firstSection >= 0 ? firstSection : 0;
            fixes.push_back({Format("Добавить extern %s", name), {{at, false, "extern " + name}}});
        }
    } else if (code.rfind("callee-saved:", 0) == 0 && inFunction) {
        std::string r = code.substr(13);
        fixes.push_back({Format("Сохранить %s: push в начале функции, pop перед каждым ret", r), wrap("push " + r, "pop " + r)});
    } else if ((code == "div-rdx" || code == "idiv-rdx") && line >= 0) {
        std::string op;
        for (const Stmt& s : st) {
            if (s.line == line && !s.ops.empty()) op = s.ops[0];
        }
        std::string prep = "xor edx, edx";
        if (code == "idiv-rdx") {  // sign extension of the size of the divisor
            bool reg = !Canon(op).empty();
            bool d32 = op.rfind("dword", 0) == 0 || (reg && (op[0] == 'e' || (op[0] == 'r' && op.back() == 'd')));
            bool d16 = op.rfind("word", 0) == 0 || (reg && (op.size() == 2 || (op[0] == 'r' && op.back() == 'w')));
            prep = d32 ? "cdq" : d16 ? "cwd" : "cqo";
        }
        fixes.push_back({Format("Вставить %s перед делением", prep), {{line, false, indentOf(line) + prep}}});
    } else if ((code == "stack-align" || code == "shadow") && inFunction) {
        int64_t add = code == "stack-align" ? 8 : 32;
        // An existing "sub rsp, N" before the call (and its "add rsp, N"): change N.
        std::optional<int64_t> n;
        std::vector<Edit> edits;
        for (size_t i = first; i < last && st[i].line < line; i++) {
            if (st[i].mnemonic == "sub" && st[i].ops.size() == 2 && st[i].ops[0] == "rsp" && (n = Number(st[i].ops[1]))) {
                edits.push_back({st[i].line, true, indentOf(st[i].line) + "sub rsp, " + std::to_string(*n + add)});
                break;
            }
        }
        if (n) {
            for (size_t i = first; i < last; i++) {
                if (st[i].mnemonic == "add" && st[i].ops.size() == 2 && st[i].ops[0] == "rsp" && Number(st[i].ops[1]) == n) {
                    edits.push_back({st[i].line, true, indentOf(st[i].line) + "add rsp, " + std::to_string(*n + add)});
                }
            }
            fixes.push_back({Format("Изменить на sub rsp, %s (и add rsp)", std::to_string(*n + add)), edits});
        } else {
            std::string sz = std::to_string(add == 32 ? 32 : 8);
            if (code == "shadow") {
                int pushes = 0;
                for (size_t i = first; i < last && st[i].line < line; i++) pushes += st[i].mnemonic == "push";
                sz = pushes % 2 ? "32" : "40";
            }
            fixes.push_back({Format("Выделить место в стеке: sub rsp, %s в начале функции, add rsp перед ret", sz),
                             wrap("sub rsp, " + sz, "add rsp, " + sz)});
        }
    }
    return fixes;
}

std::string Apply(const std::string& text, const std::vector<Edit>& edits, const std::string& eol) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (true) {
        size_t nl = text.find('\n', pos);
        lines.push_back(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    std::vector<Edit> sorted = edits;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Edit& a, const Edit& b) {
        return a.line != b.line ? a.line > b.line : a.replace > b.replace;  // bottom up; on one line: replace first
    });
    std::string cr = eol == "\r\n" ? "\r" : "";
    for (const Edit& e : sorted) {
        int at = std::clamp(e.line, 0, static_cast<int>(lines.size()));
        if (e.replace && at < static_cast<int>(lines.size())) {
            bool hadCr = !lines[at].empty() && lines[at].back() == '\r';
            lines[at] = e.text + (hadCr ? "\r" : "");
        } else {
            lines.insert(lines.begin() + at, e.text + cr);
        }
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

std::optional<CallArguments> ArgumentsOfCall(const std::string& text, int line) {
    std::vector<Stmt> st = Parse(text);
    size_t at = st.size();
    for (size_t i = 0; i < st.size(); i++) {
        if (st[i].line == line && st[i].mnemonic == "call" && !st[i].rawOps.empty()) at = i;
    }
    if (at == st.size()) return std::nullopt;
    CallArguments r;
    std::string target = st[at].rawOps[0];
    size_t imp = target.find("__imp_");
    r.function = ReadWord(imp != std::string::npos ? target.substr(imp + 6) : LTrim(target), 0);
    if (r.function.empty()) return std::nullopt;
    std::vector<std::string> names;
    r.known = Hints::Parameters(r.function, names, r.variadic, r.prototype);

    static const char* regs[] = {"rcx", "rdx", "r8", "r9"};
    std::map<int, int> setOn;  // argument index -> line
    for (size_t k = at; k-- > 0;) {
        const Stmt& p = st[k];
        if (p.mnemonic == "call" || p.mnemonic == "ret" || p.mnemonic == "retn" || p.mnemonic == "jmp") break;
        for (const std::string& w : Writes(p)) {
            for (int i = 0; i < 4; i++) {
                if (w == regs[i] || w == "xmm" + std::to_string(i)) setOn.emplace(i, p.line);
            }
        }
        if (p.mnemonic == "mov" && !p.ops.empty()) {
            if (auto slot = RspSlot(p.ops[0]); slot && *slot >= 32 && *slot % 8 == 0) setOn.emplace(4 + static_cast<int>((*slot - 32) / 8), p.line);
        }
        if (!p.label.empty()) break;
    }
    auto where = [](int i) { return i < 4 ? std::string(regs[i]) : "[rsp+" + std::to_string(32 + 8 * (i - 4)) + "]"; };
    auto lineOf = [&](int i) {
        auto it = setOn.find(i);
        return it == setOn.end() ? -1 : it->second;
    };
    if (r.known) {
        for (int i = 0; i < static_cast<int>(names.size()); i++) r.arguments.push_back({where(i), names[i], lineOf(i)});
        if (r.variadic) {
            for (const auto& [i, l] : setOn) {
                if (i >= static_cast<int>(names.size())) r.extra.push_back({where(i), "", l});
            }
        }
    } else {
        for (const auto& [i, l] : setOn) r.arguments.push_back({where(i), "", l});
    }
    return r;
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

// ---- conditions -------------------------------------------------------------------------------------------------

namespace Conditions {

std::vector<Condition> All(uint32_t f) {
    bool cf = f & 1, pf = (f >> 2) & 1, zf = (f >> 6) & 1, sf = (f >> 7) & 1, of = (f >> 11) & 1;
    auto v = [](std::initializer_list<std::pair<const char*, bool>> flags) {
        std::string s;
        for (auto [name, on] : flags) s += std::string(s.empty() ? "" : " ") + name + (on ? "=1" : "=0");
        return s;
    };
    return {
        {"o", "", "OF = 1", "переполнение (со знаком)", v({{"OF", of}}), of},
        {"no", "", "OF = 0", "нет переполнения", v({{"OF", of}}), !of},
        {"b", "c nae", "CF = 1", "меньше (без знака)", v({{"CF", cf}}), cf},
        {"ae", "nc nb", "CF = 0", "больше или равно (без знака)", v({{"CF", cf}}), !cf},
        {"e", "z", "ZF = 1", "равно (результат 0)", v({{"ZF", zf}}), zf},
        {"ne", "nz", "ZF = 0", "не равно", v({{"ZF", zf}}), !zf},
        {"be", "na", "CF = 1 или ZF = 1", "меньше или равно (без знака)", v({{"CF", cf}, {"ZF", zf}}), cf || zf},
        {"a", "nbe", "CF = 0 и ZF = 0", "больше (без знака)", v({{"CF", cf}, {"ZF", zf}}), !cf && !zf},
        {"s", "", "SF = 1", "результат отрицательный", v({{"SF", sf}}), sf},
        {"ns", "", "SF = 0", "результат неотрицательный", v({{"SF", sf}}), !sf},
        {"p", "pe", "PF = 1", "чётное число единиц в младшем байте", v({{"PF", pf}}), pf},
        {"np", "po", "PF = 0", "нечётное число единиц в младшем байте", v({{"PF", pf}}), !pf},
        {"l", "nge", "SF ≠ OF", "меньше (со знаком)", v({{"SF", sf}, {"OF", of}}), sf != of},
        {"ge", "nl", "SF = OF", "больше или равно (со знаком)", v({{"SF", sf}, {"OF", of}}), sf == of},
        {"le", "ng", "ZF = 1 или SF ≠ OF", "меньше или равно (со знаком)", v({{"ZF", zf}, {"SF", sf}, {"OF", of}}),
         zf || sf != of},
        {"g", "nle", "ZF = 0 и SF = OF", "больше (со знаком)", v({{"ZF", zf}, {"SF", sf}, {"OF", of}}), !zf && sf == of},
    };
}

std::optional<Instruction> Decode(const uint8_t* code, size_t size, uint32_t eflags, uint64_t rcx, bool is32) {
    size_t i = 0;
    bool addr32 = is32;
    while (i < size && i < 4) {  // prefixes
        uint8_t b = code[i];
        if (b == 0x67) addr32 = !is32;
        else if (b != 0x66 && b != 0xF2 && b != 0xF3 && b != 0x2E && b != 0x3E && !(!is32 && (b & 0xF0) == 0x40)) break;
        i++;
    }
    if (i >= size) return std::nullopt;
    auto all = All(eflags);
    Instruction ins;
    uint8_t op = code[i];
    if (op >= 0x70 && op <= 0x7F) {
        ins.condition = all[op & 0xF];
    } else if (op == 0x0F && i + 1 < size) {
        uint8_t op2 = code[i + 1];
        if (op2 >= 0x80 && op2 <= 0x8F) {
            ins.condition = all[op2 & 0xF];
        } else if (op2 >= 0x40 && op2 <= 0x4F) {
            ins.kind = Instruction::Kind::Move;
            ins.condition = all[op2 & 0xF];
        } else if (op2 >= 0x90 && op2 <= 0x9F) {
            ins.kind = Instruction::Kind::Set;
            ins.condition = all[op2 & 0xF];
        } else {
            return std::nullopt;
        }
    } else if (op >= 0xE0 && op <= 0xE3) {
        std::string reg = addr32 ? "ecx" : is32 ? "cx" : "rcx";
        uint64_t count = addr32 ? (rcx & 0xFFFFFFFF) : is32 ? (rcx & 0xFFFF) : rcx;
        bool zf = (eflags >> 6) & 1;
        Condition c;
        c.values = reg + " = " + std::to_string(count) + (op != 0xE2 && op != 0xE3 ? std::string(" ZF=") + (zf ? "1" : "0") : "");
        if (op == 0xE3) {
            ins.mnemonic = addr32 ? "jecxz" : is32 ? "jcxz" : "jrcxz";
            c.formula = reg + " = 0";
            c.meaning = "счётчик равен нулю";
            c.holds = count == 0;
        } else {
            ins.kind = Instruction::Kind::Loop;
            c.formula = reg + " − 1 ≠ 0";
            c.meaning = "счётчик после уменьшения не ноль";
            c.holds = count != 1;
            if (op == 0xE1) {
                c.formula += " и ZF = 1";
                c.meaning += ", и было равно";
                c.holds = c.holds && zf;
            } else if (op == 0xE0) {
                c.formula += " и ZF = 0";
                c.meaning += ", и было не равно";
                c.holds = c.holds && !zf;
            }
            ins.mnemonic = op == 0xE2 ? "loop" : op == 0xE1 ? "loope" : "loopne";
        }
        ins.condition = c;
        return ins;
    } else if (op == 0xEB || op == 0xE9) {
        ins.kind = Instruction::Kind::Unconditional;
        ins.mnemonic = "jmp";
        ins.condition.formula = "—";
        ins.condition.meaning = "переход всегда";
        ins.condition.holds = true;
        return ins;
    } else {
        return std::nullopt;
    }
    const char* prefix = ins.kind == Instruction::Kind::Move ? "cmov" : ins.kind == Instruction::Kind::Set ? "set" : "j";
    ins.mnemonic = prefix + ins.condition.name;
    return ins;
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
