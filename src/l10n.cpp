#include "common.h"

#include <unordered_map>

// English texts for the Russian UI strings. Lookup is by the exact Russian text; anything missing stays Russian
// (the instruction reference in hints.cpp is Russian only).
namespace {

#include "l10n_table.inc"

bool g_english = false;

const std::unordered_map<std::wstring, const wchar_t*>& WideTable() {
    static const std::unordered_map<std::wstring, const wchar_t*> table = [] {
        std::unordered_map<std::wstring, const wchar_t*> t;
        for (const auto& [ru, en] : kTable) t.emplace(ru, en);
        return t;
    }();
    return table;
}

const std::unordered_map<std::string, std::string>& Utf8Table() {
    static const std::unordered_map<std::string, std::string> table = [] {
        std::unordered_map<std::string, std::string> t;
        for (const auto& [ru, en] : kTable) t.emplace(Narrow(ru), Narrow(en));
        return t;
    }();
    return table;
}
}

void SetLanguage(const std::wstring& id) {
    g_english = id == L"en";
}

bool English() {
    return g_english;
}

const wchar_t* Tr(const wchar_t* russian) {
    if (!g_english || !russian) return russian;
    auto it = WideTable().find(russian);
    return it == WideTable().end() ? russian : it->second;
}

std::string TrEnglish(const std::string& russian) {
    auto it = Utf8Table().find(russian);
    return it == Utf8Table().end() ? russian : it->second;
}

std::string Tr(const std::string& russian) {
    if (!g_english) return russian;
    auto it = Utf8Table().find(russian);
    if (it != Utf8Table().end()) return it->second;
    // Messages often end with a line break: translate the text before it.
    size_t end = russian.find_last_not_of("\n");
    if (end != std::string::npos && end + 1 < russian.size()) {
        auto inner = Utf8Table().find(russian.substr(0, end + 1));
        if (inner != Utf8Table().end()) return inner->second + russian.substr(end + 1);
    }
    return russian;
}
