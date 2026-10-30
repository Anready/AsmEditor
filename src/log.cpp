#include "common.h"

#include <fstream>
#include <mutex>

namespace Log {
namespace {
std::mutex g_mutex;
bool g_enabled = [] {
    size_t n = 0;
    return getenv_s(&n, nullptr, 0, "ASMEDITOR_LOG") == 0 && n > 0;
}();
}

fs::path File() {
    return Settings::Dir() / L"asmeditor.log";
}

void Enable(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    size_t n = 0;
    bool env = getenv_s(&n, nullptr, 0, "ASMEDITOR_LOG") == 0 && n > 0;
    g_enabled = on || env;
}

bool Enabled() {
    return g_enabled;
}

// One line per call: "2026-10-30 18:56:01.123 [tid] text". The file is cut when it grows past 2 MB.
void Write(const std::string& utf8) {
    if (!g_enabled) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    fs::path file = File();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (fs::file_size(file, ec) > 2'000'000 && !ec) fs::rename(file, fs::path(file).replace_extension(L".old.log"), ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[64];
    sprintf_s(head, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
              t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    std::ofstream out(file, std::ios::binary | std::ios::app);
    std::string text = utf8;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    out << head << text << "\r\n";
}
}
