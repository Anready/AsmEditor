#include "completion.h"

#include <set>

namespace Completion {
namespace {

const char* kPreprocessor[] = {
    "%define", "%xdefine", "%idefine", "%ixdefine", "%undef", "%assign", "%iassign", "%defstr", "%deftok",
    "%strlen", "%substr", "%strcat", "%macro", "%imacro", "%endmacro", "%unmacro", "%rotate", "%rep", "%endrep",
    "%exitrep", "%include", "%use", "%pathsearch", "%if", "%ifdef", "%ifndef", "%ifidn", "%ifidni", "%ifnum",
    "%ifstr", "%ifid", "%ifempty", "%ifmacro", "%elif", "%elifdef", "%elifndef", "%else", "%endif", "%error",
    "%warning", "%fatal", "%push", "%pop", "%repl", "%local", "%line", "%clear",
};

// Functions beginners usually declare with extern (C runtime and WinAPI).
const char* kCommonFunctions[] = {
    "printf", "scanf", "puts", "putchar", "getchar", "fgets", "sprintf", "sscanf", "fprintf", "fscanf",
    "fopen", "fclose", "malloc", "calloc", "realloc", "free", "strlen", "strcpy", "strcat", "strcmp",
    "memcpy", "memset", "rand", "srand", "time", "exit", "ExitProcess", "GetStdHandle", "WriteFile",
    "ReadFile", "WriteConsoleA", "WriteConsoleW", "ReadConsoleA", "MessageBoxA", "Sleep", "GetTickCount",
    "GetLastError", "CreateFileA", "CloseHandle", "VirtualAlloc", "VirtualFree", "GetProcessHeap", "HeapAlloc",
    "HeapFree", "SetConsoleOutputCP",
};

const char* kSections[] = {".text", ".data", ".bss", ".rdata"};

const std::set<std::string> kDefiningDirectives = {
    "%define", "%xdefine", "%idefine", "%ixdefine", "%assign", "%iassign", "%macro", "%imacro",
    "%strlen", "%defstr", "%deftok", "%substr", "%strcat",
};

bool IsWordChar(unsigned char c) {
    return isalnum(c) || c == '_' || c == '.' || c == '?' || c == '@' || c == '$' || c == '#' || c == '~' || c >= 0x80;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Identifiers following position i on a line ("a, b:function, c").
std::vector<std::string> NamesAfter(const std::string& line, size_t i) {
    std::vector<std::string> names;
    while (i < line.size() && line[i] != ';') {
        while (i < line.size() && !IsWordChar(static_cast<unsigned char>(line[i])) && line[i] != ';') i++;
        size_t start = i;
        while (i < line.size() && IsWordChar(static_cast<unsigned char>(line[i]))) i++;
        if (i > start) names.push_back(line.substr(start, i - start));
        while (i < line.size() && line[i] != ',' && line[i] != ';') i++;  // skip ":function" etc.
    }
    return names;
}

std::string WordAt(const std::string& line, size_t i) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t start = i;
    while (i < line.size() && IsWordChar(static_cast<unsigned char>(line[i]))) i++;
    return line.substr(start, i - start);
}
}

std::vector<Symbol> ScanSymbols(const std::string& text) {
    std::vector<Symbol> out;
    std::vector<Token> tokens;
    std::string scope;  // last non-local label: owner of the following .local labels
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        lineNo++;
        Tokenize(line.data(), static_cast<int>(line.size()), tokens);
        for (const Token& t : tokens) {
            std::string word = line.substr(t.start, t.end - t.start);
            if (t.type == T_LABEL) {
                while (!word.empty() && (word.back() == ':' || word.back() == ' ' || word.back() == '\t')) word.pop_back();
                if (word.empty()) continue;
                bool local = word[0] == '.' && word.rfind("..", 0) != 0;
                out.push_back({word, Kind::Label, lineNo, false, local ? scope : std::string()});
                if (!local) scope = word;
            } else if (t.type == T_PREPROCESSOR && kDefiningDirectives.count(Lower(word))) {
                std::string name = WordAt(line, t.end);
                if (!name.empty()) out.push_back({name, Kind::Macro, lineNo});
            } else if (t.type == T_DIRECTIVE) {
                std::string lw = Lower(word);
                if (lw == "extern" || lw == "global" || lw == "common" || lw == "static") {
                    for (const std::string& n : NamesAfter(line, t.end)) {
                        out.push_back({n, lw == "extern" ? Kind::Function : Kind::Label, lineNo, true});
                    }
                }
            }
        }
        pos = end + 1;
    }
    return out;
}

std::string BuildList(const std::string& before, const std::string& prefix, const std::vector<Symbol>& symbols) {
    // No completion inside comments and strings.
    char quote = 0;
    for (char c : before) {
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"' || c == '`') {
            quote = c;
        } else if (c == ';') {
            return {};
        }
    }
    if (quote) return {};

    // Split the statement: drop an optional "label:" and instruction prefixes.
    std::string rest = before;
    size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) i++;
    size_t j = i;
    while (j < rest.size() && IsWordChar(static_cast<unsigned char>(rest[j]))) j++;
    size_t k = j;
    while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) k++;
    if (j > i && k < rest.size() && rest[k] == ':') rest = rest.substr(k + 1);
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : rest + " ") {
            if (IsWordChar(static_cast<unsigned char>(c)) || c == '%') {
                w += c;
            } else {
                if (!w.empty()) words.push_back(Lower(w));
                w.clear();
            }
        }
    }
    auto prefixes = SyntaxWords(WordClass::Prefix);
    std::set<std::string> prefixSet(prefixes.begin(), prefixes.end());
    while (!words.empty() && prefixSet.count(words.front())) words.erase(words.begin());

    std::vector<Symbol> candidates;
    auto addWords = [&](WordClass cls, Kind kind) {
        for (std::string& w : SyntaxWords(cls)) candidates.push_back({std::move(w), kind});
    };
    auto addSymbols = [&](std::initializer_list<Kind> kinds) {
        for (const Symbol& s : symbols) {
            for (Kind kd : kinds) {
                if (s.kind == kd) candidates.push_back(s);
            }
        }
    };

    // "msg db ...": a label written without a colon is followed by a directive or instruction.
    bool afterBareLabel = words.size() == 1 && rest.find(',') == std::string::npos &&
                          std::any_of(symbols.begin(), symbols.end(),
                                      [&](const Symbol& s) { return s.kind == Kind::Label && Lower(s.name) == words[0]; });
    if (!prefix.empty() && prefix[0] == '%') {
        for (const char* p : kPreprocessor) candidates.push_back({p, Kind::Preprocessor});
        addSymbols({Kind::Macro});
    } else if (words.empty() || afterBareLabel) {
        addWords(WordClass::Instruction, Kind::Instruction);
        addWords(WordClass::Prefix, Kind::Instruction);
        addWords(WordClass::Directive, Kind::Directive);
        addWords(WordClass::DataDirective, Kind::Directive);
        addSymbols({Kind::Macro});
    } else if (words[0] == "section" || words[0] == "segment") {
        for (const char* s : kSections) candidates.push_back({s, Kind::Section});
    } else if (words[0] == "extern") {
        for (const char* f : kCommonFunctions) candidates.push_back({f, Kind::Function});
        addSymbols({Kind::Function});
    } else if (words[0] == "global") {
        addSymbols({Kind::Label});
    } else if (words[0] == "default") {
        candidates.push_back({"rel", Kind::Keyword});
        candidates.push_back({"abs", Kind::Keyword});
    } else {
        // Operands.
        addWords(WordClass::Register, Kind::Register);
        addWords(WordClass::Keyword, Kind::Keyword);
        addSymbols({Kind::Label, Kind::Macro, Kind::Function});
    }

    // Filter by the typed prefix (case-insensitive), drop duplicates, sort.
    std::string lp = Lower(prefix);
    std::map<std::string, Symbol> unique;  // lower-case name -> first symbol
    for (Symbol& s : candidates) {
        std::string ln = Lower(s.name);
        if (ln.compare(0, lp.size(), lp) != 0 || ln == lp) continue;  // the word being typed is not a suggestion
        unique.emplace(ln, std::move(s));
    }
    if (unique.empty()) return {};
    std::string list;
    int count = 0;
    for (const auto& [ln, s] : unique) {
        if (count++ == 400) break;
        if (!list.empty()) list += ' ';
        list += s.name + "?" + std::to_string(static_cast<int>(s.kind));
    }
    return list;
}

std::vector<Occurrence> FindOccurrences(const std::string& text, const std::string& name, const std::string& scope,
                                        bool withQualifiedPrefix) {
    std::vector<Occurrence> out;
    if (name.empty()) return out;
    bool local = name[0] == '.' && name.rfind("..", 0) != 0;
    std::string qualified = local ? scope + name : std::string();
    std::vector<Token> tokens;
    std::string current;  // global label owning the current line
    size_t pos = 0;
    int lineNo = 0;
    auto isPart = [](char c) { return IsWordChar(static_cast<unsigned char>(c)); };
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lineNo++;
        Tokenize(line.data(), static_cast<int>(line.size()), tokens);

        // Text that cannot contain uses, and spans that define a name on this line.
        std::vector<std::pair<int, int>> skip, defs;
        for (const Token& t : tokens) {
            if (t.type == T_COMMENT || t.type == T_STRING) skip.push_back({t.start, t.end});
            if (t.type == T_LABEL) {
                defs.push_back({t.start, t.end});
                std::string label = line.substr(t.start, t.end - t.start);
                while (!label.empty() && (label.back() == ':' || label.back() == ' ' || label.back() == '\t')) label.pop_back();
                if (!label.empty() && (label[0] != '.' || label.rfind("..", 0) == 0)) current = label;
            }
            if (t.type == T_PREPROCESSOR && kDefiningDirectives.count(Lower(line.substr(t.start, t.end - t.start)))) {
                size_t s = line.find_first_not_of(" \t", t.end);
                if (s != std::string::npos) defs.push_back({static_cast<int>(s), static_cast<int>(s + name.size())});
            }
        }
        auto inside = [](const std::vector<std::pair<int, int>>& spans, size_t at) {
            return std::any_of(spans.begin(), spans.end(), [&](const auto& r) {
                return static_cast<int>(at) >= r.first && static_cast<int>(at) < r.second;
            });
        };
        // kind: 0 = the name itself, 1 = scope.local form, 2 = name. prefix of a qualified local label
        auto scan = [&](const std::string& word, int kind) {
            for (size_t at = line.find(word); at != std::string::npos; at = line.find(word, at + 1)) {
                bool startOk = at == 0 || !isPart(line[at - 1]);
                bool endOk = kind == 2 || at + word.size() >= line.size() || !isPart(line[at + word.size()]);
                if (!startOk || !endOk || inside(skip, at)) continue;
                if (kind == 0 && local && current != scope) continue;
                int length = static_cast<int>(kind == 2 ? name.size() : word.size());
                out.push_back({lineNo, static_cast<int>(at), length, kind == 0 && inside(defs, at), kind != 0, line});
            }
        };
        scan(name, 0);
        if (local && !scope.empty()) scan(qualified, 1);
        if (!local && withQualifiedPrefix) scan(name + ".", 2);
        pos = end + 1;
    }
    std::sort(out.begin(), out.end(), [](const Occurrence& a, const Occurrence& b) {
        return a.line != b.line ? a.line < b.line : a.column < b.column;
    });
    return out;
}

bool IsValidName(const std::string& name) {
    if (name.empty()) return false;
    unsigned char first = static_cast<unsigned char>(name[0]);
    if (!(isalpha(first) || first == '_' || first == '.' || first == '?' || first >= 0x80)) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return IsWordChar(static_cast<unsigned char>(c)); });
}

bool IsReservedWord(const std::string& name) {
    std::string lower = Lower(name);
    for (WordClass cls : {WordClass::Register, WordClass::Instruction, WordClass::Directive, WordClass::DataDirective,
                          WordClass::Keyword, WordClass::Prefix}) {
        auto words = SyntaxWords(cls);
        if (std::find(words.begin(), words.end(), lower) != words.end()) return true;
    }
    return false;
}

COLORREF KindColor(Kind kind) {
    switch (kind) {
    case Kind::Instruction: return Theme::Instruction;
    case Kind::Register: return Theme::Register;
    case Kind::Directive:
    case Kind::Section: return Theme::Directive;
    case Kind::Preprocessor: return Theme::Preprocessor;
    case Kind::Keyword: return Theme::Keyword;
    case Kind::Label: return Theme::Label;
    case Kind::Macro: return Theme::Macro;
    case Kind::Function: return Theme::Function;
    case Kind::Snippet: return Theme::Accent;
    }
    return Theme::Text;
}

}
