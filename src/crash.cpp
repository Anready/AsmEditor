#include "crash.h"

#include "version.h"

#include <dbghelp.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>

namespace {

std::atomic<bool> g_writing{false};

fs::path CrashFolder() { return Settings::Dir() / L"crashes"; }

void Write(EXCEPTION_POINTERS* ep) {
    if (g_writing.exchange(true)) return;  // a crash while writing the dump
    std::error_code ec;
    fs::create_directories(CrashFolder(), ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"crash-%04d%02d%02d-%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fs::path dump = CrashFolder() / name;
    HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                          ep ? &info : nullptr, nullptr, nullptr);
        CloseHandle(file);
    }
    // What to offer next time: the dump, the exception and where it happened in which module.
    wchar_t code[16] = L"-", where[MAX_PATH + 32] = L"-";
    if (ep && ep->ExceptionRecord) {
        swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode));
        HMODULE module = nullptr;
        auto address = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module)) {
            wchar_t path[MAX_PATH] = L"";
            GetModuleFileNameW(module, path, MAX_PATH);
            swprintf_s(where, L"%s+0x%llX", fs::path(path).filename().c_str(),
                       static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
        }
    }
    std::wofstream out(CrashFolder() / L"pending.txt", std::ios::trunc);
    out << L"dump=" << dump.wstring() << L"\ncode=" << code << L"\nwhere=" << where << L"\n";
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    Write(ep);
    return EXCEPTION_CONTINUE_SEARCH;  // Windows then ends the program as usual
}

// std::terminate, a pure virtual call, abort: no exception record, the dump shows the threads.
[[noreturn]] void Fatal() {
    Write(nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

std::wstring UrlEncode(const std::wstring& s) {
    std::string utf8 = Narrow(s), out;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            sprintf_s(buf, "%%%02X", c);
            out += buf;
        }
    }
    return Widen(out);
}
}

namespace Crash {

void Install() {
    SetUnhandledExceptionFilter(Filter);
    std::set_terminate([] { Fatal(); });
    _set_purecall_handler([] { Fatal(); });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, [](int) { Fatal(); });
}

std::optional<Report> TakePending() {
    fs::path pending = CrashFolder() / L"pending.txt";
    std::wifstream in(pending);
    if (!in) return std::nullopt;
    Report r;
    for (std::wstring line; std::getline(in, line);) {
        if (line.rfind(L"dump=", 0) == 0) r.dump = line.substr(5);
        else if (line.rfind(L"code=", 0) == 0) r.code = line.substr(5);
        else if (line.rfind(L"where=", 0) == 0) r.where = line.substr(6);
    }
    in.close();
    std::error_code ec;
    fs::remove(pending, ec);
    if (r.dump.empty()) return std::nullopt;
    return r;
}

std::wstring IssueUrl(const Report& report) {
    OSVERSIONINFOEXW os{sizeof(os)};
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) get(&os);
    std::wstring title = L"Crash: " + report.code + L" at " + report.where;
    std::wstring body = L"AsmEditor " ASMEDITOR_VERSION_TEXT_W L"\nWindows " + std::to_wstring(os.dwMajorVersion) + L"." +
                        std::to_wstring(os.dwMinorVersion) + L"." + std::to_wstring(os.dwBuildNumber) +
                        L"\nException: " + report.code + L" at " + report.where +
                        L"\n\nWhat were you doing when it closed?\n\n\n"
                        L"The crash dump is " + report.dump.filename().wstring() +
                        L" (in the AsmEditor settings folder, crashes): attach it here if you can.";
    return L"https://github.com/Anready/AsmEditor/issues/new?title=" + UrlEncode(title) + L"&body=" + UrlEncode(body);
}
}
