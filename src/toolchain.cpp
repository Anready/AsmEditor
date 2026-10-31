#include "common.h"

#include <psapi.h>

#include <chrono>
#include <fstream>
#include <future>
#include <regex>

#include "asmtools.h"
#include "project.h"

const LinkerPreset kLinkerPresets[] = {
    {L"MSVC_CRT", L"MSVC link + C runtime (printf, scanf…; точка входа main)",
     L"/nologo /subsystem:console /largeaddressaware:no /debug /incremental:no \"${obj}\" /out:\"${exe}\" "
     L"msvcrt.lib legacy_stdio_definitions.lib kernel32.lib user32.lib"},
    {L"MSVC_WINAPI", L"MSVC link, только WinAPI (без CRT, точка входа main)",
     L"/nologo /subsystem:console /entry:main /largeaddressaware:no /debug /incremental:no \"${obj}\" "
     L"/out:\"${exe}\" kernel32.lib user32.lib"},
    {L"MSVC_X86_CRT", L"MSVC link x86 (32 бит) + C runtime (-f win32; точка входа _main)",
     L"/nologo /subsystem:console /debug /incremental:no /safeseh:no \"${obj}\" /out:\"${exe}\" "
     L"msvcrt.lib legacy_stdio_definitions.lib kernel32.lib user32.lib"},
    {L"GCC", L"MinGW gcc (точка входа main)", L"\"${obj}\" -o \"${exe}\""},
    {L"CUSTOM", L"Свои параметры", nullptr},
};
const int kLinkerPresetCount = sizeof(kLinkerPresets) / sizeof(kLinkerPresets[0]);

const LinkerPreset* FindPreset(const std::wstring& id) {
    for (const LinkerPreset& p : kLinkerPresets) {
        if (id == p.id) return &p;
    }
    return &kLinkerPresets[kLinkerPresetCount - 1];
}

namespace Toolchain {

// ---- detection ------------------------------------------------------------------------------------------------

static std::wstring Env(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return n && n < 32768 ? std::wstring(buf, n) : std::wstring();
}

std::optional<fs::path> FindOnPath(const wchar_t* exe) {
    for (const std::wstring& dir : Split(Env(L"PATH"), L';')) {
        std::wstring d = dir;
        d.erase(std::remove(d.begin(), d.end(), L'"'), d.end());
        fs::path p = fs::path(d) / exe;
        if (FileExists(p.wstring())) return p;
    }
    return std::nullopt;
}

std::optional<fs::path> FindNasm() {
    if (auto p = FindOnPath(L"nasm.exe")) return p;
    std::vector<fs::path> candidates;
    if (auto v = Env(L"LOCALAPPDATA"); !v.empty()) candidates.push_back(fs::path(v) / L"bin" / L"NASM" / L"nasm.exe");
    if (auto v = Env(L"ProgramFiles"); !v.empty()) candidates.push_back(fs::path(v) / L"NASM" / L"nasm.exe");
    if (auto v = Env(L"ProgramFiles(x86)"); !v.empty()) candidates.push_back(fs::path(v) / L"NASM" / L"nasm.exe");
    if (auto v = Env(L"LOCALAPPDATA"); !v.empty()) candidates.push_back(fs::path(v) / L"NASM" / L"nasm.exe");
    for (const fs::path& c : candidates) {
        if (FileExists(c.wstring())) return c;
    }
    return std::nullopt;
}

static std::vector<fs::path> SubDirs(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) out.push_back(it->path());
    }
    return out;
}

static std::vector<int> VersionKey(const fs::path& p) {
    std::vector<int> key;
    for (const std::wstring& part : Split(p.filename().wstring(), L'.')) key.push_back(_wtoi(part.c_str()));
    return key;
}

static std::optional<fs::path> Newest(const std::vector<fs::path>& dirs, const std::function<bool(const fs::path&)>& ok) {
    std::optional<fs::path> best;
    for (const fs::path& d : dirs) {
        if (!ok(d)) continue;
        if (!best || VersionKey(d) > VersionKey(*best)) best = d;
    }
    return best;
}

std::optional<MsvcInfo> FindMsvc(bool x86) {
    const wchar_t* arch = x86 ? L"x86" : L"x64";
    std::wstring pf86 = Env(L"ProgramFiles(x86)");
    if (pf86.empty()) pf86 = L"C:\\Program Files (x86)";
    std::vector<fs::path> installs;
    fs::path vswhere = fs::path(pf86) / L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";
    if (FileExists(vswhere.wstring())) {
        std::string out;
        Exec({vswhere.wstring(), L"-all", L"-products", L"*", L"-property", L"installationPath"}, {}, nullptr, &out);
        for (const std::wstring& line : Split(Widen(out), L'\n')) installs.emplace_back(line);
    }
    for (const std::wstring& base : {Env(L"ProgramFiles"), pf86}) {
        if (base.empty()) continue;
        for (const fs::path& year : SubDirs(fs::path(base) / L"Microsoft Visual Studio")) {
            for (const fs::path& edition : SubDirs(year)) installs.push_back(edition);
        }
    }
    for (const fs::path& inst : installs) {
        auto ver = Newest(SubDirs(inst / L"VC" / L"Tools" / L"MSVC"), [&](const fs::path& v) {
            return FileExists((v / L"bin" / L"Hostx64" / arch / L"link.exe").wstring()) &&
                   FileExists((v / L"lib" / arch / L"msvcrt.lib").wstring());
        });
        if (!ver) continue;
        MsvcInfo info;
        info.link = *ver / L"bin" / L"Hostx64" / arch / L"link.exe";
        info.libPaths.push_back(*ver / L"lib" / arch);
        auto sdk = Newest(SubDirs(fs::path(pf86) / L"Windows Kits" / L"10" / L"Lib"), [&](const fs::path& v) {
            return FileExists((v / L"um" / arch / L"kernel32.lib").wstring());
        });
        if (sdk) {
            info.libPaths.push_back(*sdk / L"um" / arch);
            info.libPaths.push_back(*sdk / L"ucrt" / arch);
        }
        return info;
    }
    return std::nullopt;
}

void DetectLinker(Settings& s) {
    if (auto msvc = FindMsvc()) {
        s.linkerPath = msvc->link.wstring();
        s.linkerPreset = L"MSVC_CRT";
        s.linkerArgs = FindPreset(L"MSVC_CRT")->args;
        s.libPaths.clear();
        for (const fs::path& p : msvc->libPaths) s.libPaths.push_back(p.wstring());
        return;
    }
    if (auto gcc = FindOnPath(L"gcc.exe")) {
        s.linkerPath = gcc->wstring();
        s.linkerPreset = L"GCC";
        s.linkerArgs = FindPreset(L"GCC")->args;
        s.libPaths.clear();
    }
}

void ApplyDefaults(Settings& s) {
    bool changed = false;
    if (s.nasmPath.empty()) {
        if (auto n = FindNasm()) {
            s.nasmPath = n->wstring();
            changed = true;
        }
    }
    if (s.nasmArgs.empty()) {
        s.nasmArgs = kDefaultNasmArgs;
        changed = true;
    }
    if (s.linkerPath.empty()) {
        DetectLinker(s);
        changed = true;
    }
    if (changed) s.Save();
}

bool NasmAvailable(const Settings& s) {
    return FileExists(s.nasmPath);
}

bool LinkerAvailable(const Settings& s) {
    return FileExists(s.linkerPath);
}

std::wstring NasmVersion(const Settings& s) {
    if (!NasmAvailable(s)) return {};
    std::string out;
    Exec({s.nasmPath, L"-v"}, {}, nullptr, &out);
    std::smatch m;
    if (std::regex_search(out, m, std::regex("version\\s+(\\S+)"))) return Widen(m[1].str());
    return L"?";
}

// ---- command lines --------------------------------------------------------------------------------------------

std::vector<std::wstring> SplitArgs(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false;
    bool any = false;
    for (wchar_t c : s) {
        if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if (iswspace(c) && !quoted) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
            any = true;
        }
    }
    if (any) out.push_back(cur);
    return out;
}

// Quoting compatible with CommandLineToArgvW / the MSVC runtime.
static void AppendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += L'"';
        } else {
            cmd.append(backslashes, L'\\');
            cmd += arg[i];
        }
    }
    cmd += L'"';
}

std::wstring QuoteCommandLine(const std::vector<std::wstring>& args) {
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) cmd += L' ';
        AppendQuoted(cmd, args[i]);
    }
    return cmd;
}

std::wstring NasmArgs(const Settings& s, const RunConfig& c) {
    std::wstring base = s.nasmArgs.empty() ? kDefaultNasmArgs : s.nasmArgs;
    std::vector<std::wstring> args;
    if (c.Release() && !s.releaseNasmArgs.empty()) {
        args = SplitArgs(s.releaseNasmArgs);
    } else {
        std::vector<std::wstring> all = SplitArgs(base);
        for (size_t i = 0; i < all.size(); i++) {
            if (c.Release() && all[i] == L"-g") continue;  // Release: no debug information
            if (c.Release() && all[i] == L"-F") {
                i++;
                continue;
            }
            args.push_back(all[i]);
        }
    }
    if (c.Wsl()) {  // Linux object files: ELF with DWARF line information
        for (std::wstring& a : args) {
            if (c.Linux32() && (a == L"win64" || a == L"win32")) a = L"elf32";
            else if (c.Linux32() && (a == L"-fwin64" || a == L"-fwin32")) a = L"-felf32";
            else if (a == L"win64") a = L"elf64";
            else if (a == L"win32") a = L"elf32";
            else if (a == L"cv8") a = L"dwarf";
            else if (a == L"-fwin64") a = L"-felf64";
        }
    }
    return QuoteCommandLine(args);
}

std::wstring LinkerArgs(const Settings& s, const RunConfig& c) {
    if (!c.Release()) return s.linkerArgs;
    if (!s.releaseLinkerArgs.empty()) return s.releaseLinkerArgs;
    std::vector<std::wstring> kept;
    for (const std::wstring& a : SplitArgs(s.linkerArgs)) {
        std::wstring l = Lower(a);
        if (l == L"/debug" || l.rfind(L"/debug:", 0) == 0 || l == L"-g") continue;
        kept.push_back(a);
    }
    return QuoteCommandLine(kept);
}

std::wstring WslPath(const fs::path& windowsPath) {
    std::wstring p = NormalizePath(windowsPath).wstring();
    if (p.size() >= 2 && p[1] == L':') {
        std::wstring rest = p.substr(2);
        std::replace(rest.begin(), rest.end(), L'\\', L'/');
        return L"/mnt/" + std::wstring(1, static_cast<wchar_t>(towlower(p[0]))) + rest;
    }
    std::replace(p.begin(), p.end(), L'\\', L'/');
    return p;
}

std::optional<fs::path> FindNdisasm(const Settings& s) {
    if (!s.nasmPath.empty()) {
        fs::path p = fs::path(s.nasmPath).parent_path() / L"ndisasm.exe";
        if (FileExists(p.wstring())) return p;
    }
    return FindOnPath(L"ndisasm.exe");
}

static std::vector<std::wstring> NasmCommand(const Settings& s, const RunConfig& c, const fs::path& src,
                                             const fs::path& obj, const std::vector<fs::path>& includes) {
    std::vector<std::wstring> cmd{s.nasmPath};
    for (const std::wstring& a : SplitArgs(NasmArgs(s, c))) cmd.push_back(a);
    for (const fs::path& inc : includes) cmd.push_back(L"-I" + inc.wstring() + L"\\");
    cmd.push_back(L"-o");
    cmd.push_back(obj.wstring());
    cmd.push_back(src.wstring());
    return cmd;
}

static void ReplaceAll(std::wstring& s, const std::wstring& from, const std::wstring& to) {
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::wstring::npos; pos += to.size()) s.replace(pos, from.size(), to);
}

static bool IsMsvcLink(const Settings& s) {
    return _wcsicmp(fs::path(s.linkerPath).filename().c_str(), L"link.exe") == 0;
}

static std::vector<std::wstring> LinkCommand(const Settings& s, const RunConfig& c, const std::vector<fs::path>& objs,
                                             const fs::path& exe) {
    bool msvc = IsMsvcLink(s);
    if (c.output == L"lib") {  // static library: lib.exe next to link.exe, or ar next to gcc
        if (msvc) {
            std::vector<std::wstring> cmd{(fs::path(s.linkerPath).parent_path() / L"lib.exe").wstring(), L"/nologo",
                                          L"/out:" + exe.wstring()};
            for (const fs::path& o : objs) cmd.push_back(o.wstring());
            return cmd;
        }
        fs::path ar = fs::path(s.linkerPath).parent_path() / L"ar.exe";
        std::vector<std::wstring> cmd{FileExists(ar.wstring()) ? ar.wstring() : L"ar", L"rcs", exe.wstring()};
        for (const fs::path& o : objs) cmd.push_back(o.wstring());
        return cmd;
    }
    std::vector<std::wstring> cmd{s.linkerPath};
    bool dll = c.output == L"dll";
    for (std::wstring arg : SplitArgs(LinkerArgs(s, c))) {
        if (arg == L"${obj}" || arg == L"${objs}") {  // one argument per object file
            for (const fs::path& o : objs) cmd.push_back(o.wstring());
            continue;
        }
        if (dll && msvc && _wcsnicmp(arg.c_str(), L"/entry:", 7) == 0) arg = L"/noentry";  // a DLL without DllMain
        ReplaceAll(arg, L"${obj}", objs.empty() ? L"" : objs.front().wstring());
        ReplaceAll(arg, L"${exe}", exe.wstring());
        ReplaceAll(arg, L"${name}", exe.stem().wstring());
        ReplaceAll(arg, L"${dir}", exe.parent_path().wstring());
        cmd.push_back(arg);
    }
    if (dll) cmd.push_back(msvc ? L"/dll" : L"-shared");
    if (msvc) {
        for (const std::wstring& lib : s.libPaths) cmd.push_back(L"/LIBPATH:" + lib);
    }
    return cmd;
}

// Linux program: the ELF objects are linked inside WSL, with gcc when the program has main (C library),
// otherwise with ld (_start).
static std::vector<std::wstring> WslLinkCommand(const BuildPlan& plan, const std::vector<fs::path>& objs) {
    bool hasMain = false;
    for (const fs::path& src : plan.sources) hasMain |= Project::DefinesMain(src);
    std::vector<std::wstring> cmd{L"wsl.exe", L"--exec", hasMain ? L"gcc" : L"ld"};
    if (hasMain) cmd.push_back(L"-no-pie");
    if (plan.config.Linux32()) {  // needs gcc-multilib in WSL for the C runtime
        cmd.push_back(hasMain ? L"-m32" : L"-m");
        if (!hasMain) cmd.push_back(L"elf_i386");
    }
    if (plan.config.output == L"dll") cmd.push_back(L"-shared");
    cmd.push_back(L"-o");
    cmd.push_back(WslPath(plan.exe));
    for (const fs::path& o : objs) cmd.push_back(WslPath(o));
    return cmd;
}

// Pre-/post-build commands: one per line, run with cmd.exe in the project folder.
static bool RunUserCommands(const std::wstring& commands, const fs::path& cwd, const char* what, const Sink& sink) {
    for (const std::wstring& line : Split(commands, L'\n')) {
        sink(std::string(what) + ": " + Narrow(line) + "\n", Out::Command);
        int code = Exec({L"cmd.exe", L"/d", L"/s", L"/c", line}, cwd,
                        [&](const std::string& out) { sink(out + "\n", Out::Normal); }, nullptr);
        if (code != 0) {
            sink(std::string(what) + ": " + Tr(std::string("команда завершилась с кодом")) + " " + std::to_string(code) +
                     "\n", Out::Error);
            return false;
        }
    }
    return true;
}

// ---- processes ------------------------------------------------------------------------------------------------

bool Spawn(const std::wstring& commandLine, const fs::path& cwd, HANDLE in, HANDLE out, HANDLE err, DWORD flags,
           PROCESS_INFORMATION& pi) {
    // Restrict inheritance to exactly these handles so concurrent spawns never leak pipe ends to each other.
    HANDLE list[3];
    int count = 0;
    for (HANDLE h : {in, out, err}) {
        bool dup = false;
        for (int i = 0; i < count; i++) dup |= list[i] == h;
        if (h && !dup) list[count++] = h;
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> buffer(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) return false;
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, count * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = attrs;
    std::wstring cmd = commandLine;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags | EXTENDED_STARTUPINFO_PRESENT,
                             nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    return ok != FALSE;
}

bool SpawnInConsole(const std::wstring& commandLine, const fs::path& cwd, HPCON console, DWORD flags,
                    PROCESS_INFORMATION& pi, const std::wstring& environment) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> buffer(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) return false;
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(console), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    std::wstring cmd = commandLine;
    std::wstring env = environment;
    if (!env.empty()) flags |= CREATE_UNICODE_ENVIRONMENT;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags | EXTENDED_STARTUPINFO_PRESENT,
                             env.empty() ? nullptr : env.data(), cwd.empty() ? nullptr : cwd.c_str(),
                             &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    return ok != FALSE;
}

std::wstring EnvironmentBlock(const std::wstring& extraLines) {
    struct Less {
        bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, std::wstring, Less> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry = p;
            size_t eq = entry.find(L'=', 1);  // "=C:=C:\..." entries start with '='
            if (eq != std::wstring::npos) vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const std::wstring& line : Split(extraLines, L'\n')) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        std::wstring name = Trim(line.substr(0, eq)), value = line.substr(eq + 1);
        if (value.empty()) vars.erase(name);
        else vars[name] = value;
    }
    std::wstring out;
    for (const auto& [name, value] : vars) out += name + L"=" + value + L'\0';
    out += L'\0';
    return out;
}

std::string ProcessStats(HANDLE process, ULONGLONG startTick) {
    char buf[160];
    double wall = (GetTickCount64() - startTick) / 1000.0;
    FILETIME created, exited, kernel, user;
    double cpu = -1;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        auto ticks = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        cpu = (ticks(kernel) + ticks(user)) / 1e7;
    }
    PROCESS_MEMORY_COUNTERS mem{sizeof(mem)};
    bool haveMem = K32GetProcessMemoryInfo(process, &mem, sizeof(mem)) != FALSE;
    std::string s;
    sprintf_s(buf, "%s %.2f %s", Tr(std::string("время")).c_str(), wall, Tr(std::string("с")).c_str());
    s = buf;
    if (cpu >= 0) {
        sprintf_s(buf, " (%s %.2f %s)", Tr(std::string("процессор")).c_str(), cpu, Tr(std::string("с")).c_str());
        s += buf;
    }
    if (haveMem) {
        sprintf_s(buf, " · %s %.1f %s", Tr(std::string("пик памяти")).c_str(), mem.PeakWorkingSetSize / (1024.0 * 1024.0),
                  Tr(std::string("МБ")).c_str());
        s += buf;
    }
    return s;
}

void SetConsoleCodePage(HPCON console, UINT codePage) {
    // The code page belongs to the console, not to a process: set it the way a terminal user would, with chcp
    // run in the same pseudoconsole right before the program starts.
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"cmd.exe /d /c chcp " + std::to_wstring(codePage) + L" >nul";
    if (!SpawnInConsole(cmd, {}, console, 0, pi)) return;
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

int Exec(const std::vector<std::wstring>& cmd, const fs::path& cwd, const std::function<void(const std::string&)>& onLine,
         std::string* capture) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi{};
    bool started = Spawn(QuoteCommandLine(cmd), cwd, nul, wr, wr, CREATE_NO_WINDOW, pi);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        std::string msg = Tr(std::string("Не удалось запустить ")) + Narrow(cmd.empty() ? L"" : cmd[0]) + " (" +
                          Tr(std::string("код")) + " " + std::to_string(GetLastError()) + ")";
        if (onLine) onLine(msg);
        if (capture) *capture += msg + "\n";
        return -1;
    }
    std::string pending;
    char buf[4096];
    DWORD n;
    auto flushLine = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string utf8 = Narrow(Widen(line, CP_ACP));  // tools print in the ANSI code page
        if (capture) *capture += utf8 + "\n";
        if (onLine) onLine(utf8);
    };
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            flushLine(pending.substr(0, pos));
            pending.erase(0, pos + 1);
        }
    }
    if (!pending.empty()) flushLine(pending);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// ---- assembler output -----------------------------------------------------------------------------------------

std::vector<Diagnostic> ParseNasm(const std::string& output) {
    static const std::wregex re(L"^(.+?):(\\d+):(?:\\d+:)?\\s*(error|warning|fatal|panic)\\s*:\\s*(.*)$",
                                std::regex::icase);
    std::vector<Diagnostic> out;
    for (const std::wstring& line : Split(Widen(output), L'\n')) {
        std::wsmatch m;
        if (!std::regex_match(line, m, re)) continue;
        Diagnostic d;
        d.file = NormalizePath(m[1].str());
        d.line = _wtoi(m[2].str().c_str());
        d.severity = _wcsicmp(m[3].str().c_str(), L"warning") == 0 ? Severity::Warning : Severity::Error;
        d.message = m[4].str();
        std::wsmatch u;
        static const std::wregex undefined(L"symbol `([^']+)' not defined");
        if (std::regex_search(d.message, u, undefined)) d.code = "undefined:" + Narrow(u[1].str());
        out.push_back(d);
    }
    return out;
}

static Out Classify(const std::string& line) {
    std::string l = line;
    for (char& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (l.find("error") != std::string::npos || l.find("fatal") != std::string::npos ||
        l.find("panic") != std::string::npos) return Out::Error;
    if (l.find("warning") != std::string::npos) return Out::Warning;
    return Out::Normal;
}

// Object file for a source: build/obj/<path relative to the root>.obj, so equal names in subfolders don't clash.
static fs::path ObjectPath(const BuildPlan& plan, const fs::path& src) {
    fs::path rel = IsUnder(src, plan.root) ? NormalizePath(src).lexically_relative(NormalizePath(plan.root)) : src.filename();
    rel.replace_extension(plan.config.Wsl() ? L".o" : L".obj");
    return plan.outDir / L"obj" / rel;
}

static bool UpToDate(const fs::path& obj, const fs::path& src, const std::vector<fs::path>& includes) {
    std::error_code ec;
    auto objTime = fs::last_write_time(obj, ec);
    if (ec) return false;
    std::vector<fs::path> inputs = Project::Dependencies(src, includes);
    inputs.push_back(src);
    for (const fs::path& in : inputs) {
        auto t = fs::last_write_time(in, ec);
        if (ec || t > objTime) return false;
    }
    return true;
}

BuildResult Build(const Settings& s, const BuildPlan& plan, const Sink& sink) {
    auto started = std::chrono::steady_clock::now();
    BuildResult result;
    std::error_code ec;
    fs::create_directories(plan.outDir / L"obj", ec);
    auto relName = [&](const fs::path& f) {
        return Narrow((IsUnder(f, plan.root) ? NormalizePath(f).lexically_relative(NormalizePath(plan.root)) : f.filename())
                          .wstring());
    };

    // Object files built with other NASM arguments are stale.
    fs::path stamp = plan.outDir / L"obj" / L".nasm-args";
    std::string args = Narrow(NasmArgs(s, plan.config));
    std::string oldArgs;
    {
        std::ifstream in(stamp, std::ios::binary);
        std::getline(in, oldArgs);
    }
    bool force = plan.rebuild || oldArgs != args;

    if (!plan.config.preBuild.empty() && !RunUserCommands(plan.config.preBuild, plan.root, "pre-build", sink)) {
        return result;
    }
    size_t n = plan.sources.size();
    sink(n == 1 ? Tr(std::string("Сборка ")) + Narrow(plan.sources[0].wstring()) + "\n"
                : Tr(std::string("Сборка ")) + std::to_string(n) + Tr(std::string(" файлов → ")) +
                      Narrow(plan.exe.filename().wstring()) + "\n",
         Out::Info);

    struct Job {
        fs::path src, obj;
        bool skip = false;
        int code = 0;
        std::string log;
    };
    std::vector<Job> jobs(n);
    for (size_t i = 0; i < n; i++) {
        jobs[i].src = plan.sources[i];
        jobs[i].obj = ObjectPath(plan, plan.sources[i]);
        std::vector<fs::path> includes{plan.sources[i].parent_path()};
        includes.insert(includes.end(), plan.includes.begin(), plan.includes.end());
        jobs[i].skip = !force && UpToDate(jobs[i].obj, jobs[i].src, includes);
    }

    // Assemble in parallel (a few NASM processes at a time), report in source order.
    size_t parallel = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 4);
    for (size_t first = 0; first < n; first += parallel) {
        std::vector<std::future<void>> running;
        for (size_t i = first; i < std::min(n, first + parallel); i++) {
            if (jobs[i].skip) continue;
            running.push_back(std::async(std::launch::async, [&s, &plan, &job = jobs[i]] {
                std::error_code e;
                fs::create_directories(job.obj.parent_path(), e);
                std::vector<fs::path> includes{job.src.parent_path()};
                includes.insert(includes.end(), plan.includes.begin(), plan.includes.end());
                job.code = Exec(NasmCommand(s, plan.config, job.src, job.obj, includes), job.src.parent_path(), nullptr, &job.log);
            }));
        }
        for (auto& f : running) f.get();
        for (size_t i = first; i < std::min(n, first + parallel); i++) {
            const Job& job = jobs[i];
            std::string counter = n > 1 ? "[" + std::to_string(i + 1) + "/" + std::to_string(n) + "] " : "";
            if (job.skip) {
                sink(counter + relName(job.src) + Tr(std::string(" — без изменений")) + "\n", Out::Command);
                continue;
            }
            if (n > 1) sink(counter + relName(job.src) + "\n", Out::Normal);
            else sink("> " + Narrow(QuoteCommandLine(NasmCommand(s, plan.config, job.src, job.obj, {job.src.parent_path()}))) + "\n", Out::Command);
            for (const std::wstring& line : Split(Widen(job.log), L'\n')) {
                std::string l = Narrow(line);
                sink(l + "\n", Classify(l));
            }
            auto diags = ParseNasm(job.log);
            result.diagnostics.insert(result.diagnostics.end(), diags.begin(), diags.end());
        }
    }
    size_t failed = std::count_if(jobs.begin(), jobs.end(), [](const Job& j) { return !j.skip && j.code != 0; });
    if (failed) {
        sink(Tr(std::string("Ассемблирование завершилось с ошибками, файлов с ошибками: ")) + std::to_string(failed) + "\n", Out::Error);
        return result;
    }
    {
        std::ofstream out(stamp, std::ios::binary | std::ios::trunc);
        out << args;
    }

    if (!plan.config.Wsl() && !LinkerAvailable(s)) {
        sink(Tr(std::string("Линкер не найден — укажите его в настройках.\n")), Out::Error);
        return result;
    }
    std::vector<fs::path> objs;
    for (const Job& j : jobs) objs.push_back(j.obj);
    auto linkCmd = plan.config.Wsl() ? WslLinkCommand(plan, objs) : LinkCommand(s, plan.config, objs, plan.exe);
    sink("> " + Narrow(QuoteCommandLine(linkCmd)) + "\n", Out::Command);
    auto echo = [&](const std::string& line) { sink(line + "\n", Classify(line)); };
    int code = Exec(linkCmd, plan.outDir, echo, nullptr);
    if (code != 0) {
        sink(Tr(std::string("Линковка завершилась с ошибкой, код ")) + std::to_string(code) + "\n", Out::Error);
        return result;
    }
    if (!plan.config.postBuild.empty() && !RunUserCommands(plan.config.postBuild, plan.root, "post-build", sink)) {
        return result;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    sink(Tr(std::string("Сборка успешна за ")) + std::to_string(ms) + " " + Tr(std::string("мс")) + " → " +
             Narrow(plan.exe.wstring()) + "\n", Out::Success);
    result.ok = true;
    result.exe = plan.exe;
    return result;
}

std::vector<Diagnostic> Check(const Settings& s, const RunConfig& config, const std::string& text, int id,
                              const std::vector<fs::path>& includes) {
    wchar_t tmp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH + 1, tmp);
    fs::path dir = fs::path(tmp) / L"asmeditor-live";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path src = dir / (L"live" + std::to_wstring(id) + L".asm");
    fs::path obj = dir / (L"live" + std::to_wstring(id) + L".obj");
    {
        std::ofstream out(src, std::ios::binary | std::ios::trunc);
        out.write(text.data(), (std::streamsize)text.size());
    }
    std::string log;
    RunConfig checkConfig = config;
    checkConfig.profile = L"Debug";
    Exec(NasmCommand(s, checkConfig, src, obj, includes), dir, nullptr, &log);
    fs::remove(obj, ec);
    std::vector<Diagnostic> result;
    for (Diagnostic& d : ParseNasm(log)) {
        if (_wcsicmp(d.file.filename().c_str(), src.filename().c_str()) == 0) {
            d.file.clear();
            result.push_back(std::move(d));
        }
    }
    if (s.stackCheck && !config.Wsl()) {  // the Win64 calling convention: stack alignment and shadow space
        for (Diagnostic& d : StackCheck::Analyze(text)) result.push_back(std::move(d));
        for (Diagnostic& d : Analyzer::Analyze(text)) result.push_back(std::move(d));
    }
    return result;
}

std::wstring DescribeExitCode(DWORD code) {
    const wchar_t* name = nullptr;
    switch (code) {
    case 0xC0000005: name = L"STATUS_ACCESS_VIOLATION — обращение к недопустимому адресу памяти"; break;
    case 0xC00000FD: name = L"STATUS_STACK_OVERFLOW — переполнение стека"; break;
    case 0xC0000094: name = L"STATUS_INTEGER_DIVIDE_BY_ZERO — деление на ноль"; break;
    case 0xC0000095: name = L"STATUS_INTEGER_OVERFLOW — переполнение при делении"; break;
    case 0xC000001D: name = L"STATUS_ILLEGAL_INSTRUCTION — недопустимая инструкция"; break;
    case 0xC0000096: name = L"STATUS_PRIVILEGED_INSTRUCTION — привилегированная инструкция"; break;
    case 0xC0000409: name = L"STATUS_STACK_BUFFER_OVERRUN"; break;
    case 0xC0000135: name = L"STATUS_DLL_NOT_FOUND — не найдена DLL"; break;
    case 0xC0000139: name = L"STATUS_ENTRYPOINT_NOT_FOUND"; break;
    case 0x80000003: name = L"STATUS_BREAKPOINT — сработал int3"; break;
    case 0x80000002: name = L"STATUS_DATATYPE_MISALIGNMENT"; break;
    }
    wchar_t hex[16];
    swprintf_s(hex, L"0x%08X", code);
    std::wstring dec = std::to_wstring(static_cast<int>(code));
    if (name) return dec + L" (" + hex + L", " + Tr(name) + L")";
    if (code > 255) return dec + L" (" + hex + L")";
    return dec;
}

}  // namespace Toolchain
