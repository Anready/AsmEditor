#include "project.h"

#include <fstream>
#include <sstream>

namespace Project {
namespace {

const std::set<std::wstring> kSkippedDirs = {L"build", L".asmeditor", L".git", L".idea", L".vs", L".vscode"};

std::string ReadText(const fs::path& p) {
    std::error_code ec;
    if (fs::file_size(p, ec) > 4'000'000 || ec) return {};
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Names given to %include on each line ("file", 'file' or <file>).
std::vector<std::string> IncludeNames(const std::string& text) {
    std::vector<std::string> names;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        size_t i = text.find_first_not_of(" \t", pos);
        if (i != std::string::npos && i < end && end - i > 8 && _strnicmp(text.c_str() + i, "%include", 8) == 0) {
            size_t open = text.find_first_of("\"'<", i + 8);
            if (open != std::string::npos && open < end) {
                char close = text[open] == '<' ? '>' : text[open];
                size_t stop = text.find(close, open + 1);
                if (stop != std::string::npos && stop < end) names.push_back(text.substr(open + 1, stop - open - 1));
            }
        }
        pos = end + 1;
    }
    return names;
}

std::optional<fs::path> Resolve(const std::string& name, const fs::path& from, const std::vector<fs::path>& dirs) {
    fs::path rel = fs::path(Widen(name));
    std::error_code ec;
    if (fs::exists(from.parent_path() / rel, ec)) return NormalizePath(from.parent_path() / rel);
    for (const fs::path& d : dirs) {
        if (fs::exists(d / rel, ec)) return NormalizePath(d / rel);
    }
    return std::nullopt;
}

bool DefinesMain(const std::string& text) {
    std::vector<Token> tokens;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        Tokenize(text.data() + pos, static_cast<int>(end - pos), tokens);
        for (const Token& t : tokens) {
            if (t.type != T_LABEL) continue;
            std::string name = text.substr(pos + t.start, t.end - t.start);
            while (!name.empty() && (name.back() == ':' || name.back() == ' ' || name.back() == '\t')) name.pop_back();
            if (name == "main" || name == "wmain" || name == "WinMain") return true;
        }
        pos = end + 1;
    }
    return false;
}

void Walk(const fs::path& dir, int depth, std::vector<fs::path>& out) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (depth < 16 && !kSkippedDirs.count(Lower(it->path().filename().wstring()))) Walk(it->path(), depth + 1, out);
        } else if (IsSourceFile(it->path())) {
            out.push_back(NormalizePath(it->path()));
        }
    }
}
}

std::wstring Key(const fs::path& p) {
    return Lower(NormalizePath(p).wstring());
}

bool DefinesMain(const fs::path& file) {
    return DefinesMain(ReadText(file));
}

bool IsSourceFile(const fs::path& p) {
    std::wstring ext = Lower(p.extension().wstring());
    return ext == L".asm" || ext == L".s" || ext == L".nasm";
}

std::vector<fs::path> Dependencies(const fs::path& file, const std::vector<fs::path>& includeDirs) {
    std::vector<fs::path> out;
    std::set<std::wstring> seen{Key(file)};
    std::vector<fs::path> stack{file};
    while (!stack.empty()) {
        fs::path f = stack.back();
        stack.pop_back();
        for (const std::string& name : IncludeNames(ReadText(f))) {
            auto dep = Resolve(name, f, includeDirs);
            if (!dep || !seen.insert(Key(*dep)).second) continue;
            out.push_back(*dep);
            stack.push_back(*dep);
        }
    }
    return out;
}

Sources Collect(const fs::path& root, const fs::path& target, const std::set<std::wstring>& excluded) {
    std::vector<fs::path> all;
    Walk(root, 0, all);
    std::sort(all.begin(), all.end());

    // Files included by other files are part of those files, not separate objects.
    std::set<std::wstring> included;
    std::map<std::wstring, bool> hasMain;
    for (const fs::path& f : all) {
        std::string text = ReadText(f);
        hasMain[Key(f)] = DefinesMain(text);
        for (const std::string& name : IncludeNames(text)) {
            if (auto dep = Resolve(name, f, {root})) included.insert(Key(*dep));
        }
    }

    Sources result;
    result.files.push_back(NormalizePath(target));
    std::wstring targetKey = Key(target);
    bool targetHasMain = hasMain.count(targetKey) ? hasMain[targetKey] : DefinesMain(ReadText(target));
    for (const fs::path& f : all) {
        std::wstring k = Key(f);
        if (k == targetKey || included.count(k) || excluded.count(k)) continue;
        if (targetHasMain && hasMain[k]) {
            result.otherPrograms.push_back(f);  // a separate program in the same folder
            continue;
        }
        result.files.push_back(f);
    }
    return result;
}

}
