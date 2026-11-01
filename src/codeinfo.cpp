#include "codeinfo.h"

#include "asmtools.h"

#include <set>

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimAscii(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The size a register names: "qword" for rax, "dword" for eax… (empty for other words).
std::string RegisterSize(const std::string& r) {
    static const std::set<std::string> q = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10",
                                            "r11", "r12", "r13", "r14", "r15"};
    static const std::set<std::string> d = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "r8d", "r9d",
                                            "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
    static const std::set<std::string> w = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "r8w", "r9w", "r10w",
                                            "r11w", "r12w", "r13w", "r14w", "r15w"};
    static const std::set<std::string> b = {"al", "bl", "cl", "dl", "ah", "bh", "ch", "dh", "sil", "dil", "bpl",
                                            "spl", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
    if (q.count(r)) return "qword";
    if (d.count(r)) return "dword";
    if (w.count(r)) return "word";
    if (b.count(r)) return "byte";
    if (r.rfind("xmm", 0) == 0) return "oword";
    if (r.rfind("ymm", 0) == 0) return "yword";
    return {};
}

// Operands of an instruction, split at the commas outside brackets and quotes.
std::vector<std::string> SplitOperands(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"' || c == '`') {
            quote = c;
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
        } else if (c == ',' && depth == 0) {
            out.push_back(TrimAscii(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!TrimAscii(cur).empty() || !out.empty()) out.push_back(TrimAscii(cur));
    return out;
}

// "mov [x], eax": the memory operand has no size of its own, the register gives it.
std::string OperandSize(const std::string& line) {
    std::vector<Token> tokens;
    Tokenize(line.data(), static_cast<int>(line.size()), tokens);
    size_t mn = 0;
    while (mn < tokens.size() && tokens[mn].type != T_INSTRUCTION) mn++;
    if (mn == tokens.size()) return {};
    static const std::set<std::string> kInstructions = {"mov", "add", "sub", "and", "or", "xor", "cmp", "test",
                                                        "adc", "sbb", "xchg", "movdqa", "movdqu", "movaps", "movups"};
    std::string mnemonic = Lower(line.substr(tokens[mn].start, tokens[mn].end - tokens[mn].start));
    if (!kInstructions.count(mnemonic)) return {};
    int end = static_cast<int>(line.size());
    for (const Token& t : tokens) {
        if (t.type == T_COMMENT) end = std::min(end, t.start);
    }
    std::vector<std::string> ops = SplitOperands(line.substr(tokens[mn].end, std::max(0, end - tokens[mn].end)));
    if (ops.size() != 2) return {};
    static const char* const kSizes[] = {"byte", "word", "dword", "qword", "tword", "oword", "yword", "zword"};
    std::string size;
    bool memory = false;
    for (const std::string& op : ops) {
        std::string l = Lower(op);
        if (l.find('[') != std::string::npos) {
            std::string before = l.substr(0, l.find('['));
            for (const char* s : kSizes) {
                if (before.find(s) != std::string::npos) return {};  // its size is written
            }
            memory = true;
        } else {
            size = RegisterSize(l);
        }
    }
    return memory ? size : std::string();
}
}

namespace Inlay {

std::vector<std::string> Registers(const std::string& line) {
    std::vector<Token> tokens;
    Tokenize(line.data(), static_cast<int>(line.size()), tokens);
    std::vector<std::string> out;
    for (const Token& t : tokens) {
        if (t.type != T_REGISTER) continue;
        std::string r = Lower(line.substr(t.start, t.end - t.start));
        if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    }
    return out;
}

std::map<int, std::string> Hints(const std::string& text) {
    std::map<int, std::string> hints;
    auto add = [&](int line, const std::string& hint) {
        std::string& h = hints[line];
        if (h.find(hint) != std::string::npos) return;
        if (!h.empty()) h += "  ·  ";
        h += hint;
    };
    std::vector<std::string> lines = LineDiff::SplitLines(text);
    std::vector<Token> tokens;
    for (int i = 0; i < static_cast<int>(lines.size()); i++) {
        const std::string& line = lines[i];
        Tokenize(line.data(), static_cast<int>(line.size()), tokens);
        bool call = false;
        for (const Token& t : tokens) {
            if (t.type == T_INSTRUCTION) {
                call = Lower(line.substr(t.start, t.end - t.start)) == "call";
                break;
            }
        }
        if (call) {
            if (auto a = Analyzer::ArgumentsOfCall(text, i); a && a->known) {
                for (const Analyzer::Argument& arg : a->arguments) {
                    if (arg.setOnLine >= 0 && !arg.name.empty()) add(arg.setOnLine, arg.name + " → " + a->function);
                }
                if (!a->prototype.empty()) add(i, a->prototype);
            }
        }
        std::string size = OperandSize(line);
        if (!size.empty()) add(i, size);
    }
    return hints;
}
}

namespace LineDiff {

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        size_t len = end - pos;
        if (len && text[pos + len - 1] == '\r') len--;
        lines.push_back(text.substr(pos, len));
        if (end == text.size()) break;
        pos = end + 1;
    }
    return lines;
}

std::map<int, Change> Compare(const std::vector<std::string>& base, const std::vector<std::string>& now) {
    auto strip = [](const std::string& s) { return !s.empty() && s.back() == '\r' ? s.substr(0, s.size() - 1) : s; };
    size_t pre = 0;
    while (pre < base.size() && pre < now.size() && strip(base[pre]) == strip(now[pre])) pre++;
    size_t suf = 0;
    while (suf < base.size() - pre && suf < now.size() - pre &&
           strip(base[base.size() - 1 - suf]) == strip(now[now.size() - 1 - suf])) {
        suf++;
    }
    std::vector<std::string> a, b;
    for (size_t i = pre; i < base.size() - suf; i++) a.push_back(strip(base[i]));
    for (size_t i = pre; i < now.size() - suf; i++) b.push_back(strip(now[i]));
    int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());

    // Myers' diff of the middle: the lines both keep, as (line of a, line of b) pairs.
    std::vector<std::pair<int, int>> same;
    if (n && m) {
        int max = n + m;
        std::vector<int> v(2 * static_cast<size_t>(max) + 2, 0);
        std::vector<std::vector<int>> trace;
        bool done = false;
        for (int d = 0; d <= max && !done; d++) {
            trace.push_back(v);
            if (trace.size() * v.size() > 8'000'000) break;  // too different: every line counts as changed
            for (int k = -d; k <= d; k += 2) {
                int x = (k == -d || (k != d && v[k - 1 + max] < v[k + 1 + max])) ? v[k + 1 + max] : v[k - 1 + max] + 1;
                int y = x - k;
                while (x < n && y < m && a[x] == b[y]) {
                    x++;
                    y++;
                }
                v[k + max] = x;
                if (x >= n && y >= m) {
                    done = true;
                    break;
                }
            }
        }
        if (done) {
            int x = n, y = m;
            for (int d = static_cast<int>(trace.size()) - 1; d >= 0; d--) {
                const std::vector<int>& t = trace[d];
                int k = x - y;
                int prevK = (k == -d || (k != d && t[k - 1 + max] < t[k + 1 + max])) ? k + 1 : k - 1;
                int prevX = t[prevK + max], prevY = prevX - prevK;
                while (x > prevX && y > prevY) {
                    same.push_back({x - 1, y - 1});
                    x--;
                    y--;
                }
                x = prevX;
                y = prevY;
            }
            std::reverse(same.begin(), same.end());
        }
    }

    std::map<int, Change> out;
    int last = static_cast<int>(now.size()) - 1;
    same.push_back({n, m});
    int i = 0, j = 0;
    for (auto [x, y] : same) {
        int removed = x - i, added = y - j;
        for (int k = 0; k < added; k++) out[static_cast<int>(pre) + j + k] = k < removed ? Change::Modified : Change::Added;
        if (!added && removed && last >= 0) out.emplace(std::min(static_cast<int>(pre) + y, last), Change::Deleted);
        i = x + 1;
        j = y + 1;
    }
    return out;
}
}
