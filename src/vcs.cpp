#include "vcs.h"

namespace {

bool ValidUtf8(const std::string& s) {
    return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}
}

namespace Vcs {

std::optional<std::string> HeadText(const fs::path& file) {
    static const std::optional<fs::path> git = Toolchain::FindOnPath(L"git.exe");
    if (!git || file.empty()) return std::nullopt;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return std::nullopt;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi{};
    std::wstring spec = L"HEAD:./" + file.filename().wstring();
    bool started = Toolchain::Spawn(Toolchain::QuoteCommandLine({git->wstring(), L"show", spec}), file.parent_path(), nul, wr, nul,
                         CREATE_NO_WINDOW, pi);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        return std::nullopt;
    }
    std::string out;
    char buf[8192];
    DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 10'000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) return std::nullopt;
    if (out.rfind("\xEF\xBB\xBF", 0) == 0) out.erase(0, 3);
    if (!ValidUtf8(out)) out = Narrow(Widen(out, 1251));  // a windows-1251 file, as the editor reads it
    return out;
}
}
