#pragma once

#include "debugger.h"

#include <deque>

// gdb's machine interface (MI2): the records gdb prints, and a session that drives gdb inside WSL for Linux
// programs. The Debugger forwards its calls to the session, so the editor's panels (registers, flags, memory,
// call stack, watches, breakpoints with conditions and logpoints) work the same as for Windows programs.
namespace Mi {

struct Value {
    enum class Kind { String, Tuple, List } kind = Kind::String;
    std::string text;                                   // String
    std::vector<std::pair<std::string, Value>> fields;  // Tuple, or a List of name=value results
    std::vector<Value> items;                           // List of values
    const Value* Get(const std::string& name) const;
    std::string Str(const std::string& name) const;     // "" when missing or not a string
};

struct Record {
    char type = 0;      // '^' result, '*' exec, '+' status, '=' notify, '~' console, '@' target, '&' log
    int token = -1;
    std::string klass;  // "done", "error", "running", "stopped"...
    Value results;      // a Tuple of the name=value results
    std::string text;   // stream records: the decoded string
};

// One line of gdb output. Nothing for the "(gdb)" prompt and for lines that are not MI records.
std::optional<Record> Parse(const std::string& line);
std::string Quote(const std::string& s);              // a C string for an MI command
fs::path FromWsl(const std::string& linuxPath);       // "/mnt/d/x/y.asm" -> "D:\x\y.asm" (other paths unchanged)
std::string GdbExpression(const std::string& expr);   // the editor's syntax -> gdb's: rax -> $rax, [x] -> *(long*)(x)
// "log: a={rax} b={[x]:x}" -> dprintf format and arguments ("a=%ld b=%#lx", {"$rax", "*(long*)(x)"}).
std::pair<std::string, std::vector<std::string>> LogFormat(const std::string& message);
uint64_t ParseNumber(const std::string& s);           // "0x401000", "42", "-1"; the first number in the text
}

class GdbSession {
public:
    GdbSession(Debugger& owner, HWND notify);
    ~GdbSession();

    // gdb --interpreter=mi2 in WSL; the program's terminal is the pseudo-terminal `tty` (from the editor's
    // terminal pane, see MainWindow::RunInWsl).
    bool Start(const LaunchOptions& launch, const std::string& tty, const Debugger::Breakpoints& breakpoints,
               bool stopAtEntry, std::optional<SourceLocation> runTo, std::wstring& error);
    void Exec(const std::string& command);  // -exec-continue, -exec-next...
    void Interrupt();
    void Stop();
    bool RunTo(const SourceLocation& where);
    void SetBreakpoints(const fs::path& file, const std::map<int, std::string>& lines);

    size_t ReadMemory(uint64_t address, void* buffer, size_t size);
    bool WriteMemory(uint64_t address, const void* data, size_t size);
    bool SetRegister(const std::string& name, uint64_t value);
    bool SetNextStatement(const SourceLocation& where, std::wstring& error);
    std::optional<uint64_t> LookupSymbol(const std::string& name);
    std::wstring SymbolAt(uint64_t address);
    SourceLocation LocationOf(uint64_t address);
    std::optional<uint64_t> AddressOfLine(const SourceLocation& where);
    std::vector<Debugger::Frame> CallStack();
    bool AddDataWatch(const DataWatch& watch, std::wstring& error);
    void RemoveDataWatch(size_t index);
    std::vector<DataWatch> DataWatches();

private:
    // Sends "<token>command" and waits for its ^done / ^error (from the reader thread).
    std::optional<Mi::Record> Command(const std::string& command, DWORD timeoutMs = 10000);
    std::string Console(const std::string& command);  // a CLI command's output (-interpreter-exec console)
    void Reader();
    void Controller();
    void OnStopped(const Mi::Record& r);
    void ReadRegisters();
    void InsertBreakpoints(const std::wstring& key, const std::map<int, std::string>& lines);
    fs::path Resolve(const std::string& file, const std::string& fullname);

    Debugger& d_;
    HWND notify_;
    HANDLE process_ = nullptr, in_ = nullptr, out_ = nullptr;
    std::thread reader_, controller_;
    std::mutex mutex_;
    std::condition_variable cv_;
    int nextToken_ = 1;
    std::map<int, Mi::Record> results_;
    std::deque<Mi::Record> events_;
    std::atomic<bool> done_{false};
    std::mutex commandMutex_;  // one command at a time
    std::vector<std::string> registerNames_;
    std::map<std::wstring, std::vector<int>> fileBreakpoints_;  // Key(path) -> gdb breakpoint numbers
    std::map<std::wstring, fs::path> sources_;                  // file name (lower case) -> Windows path
    std::vector<std::pair<int, DataWatch>> watches_;            // gdb number, watch
    fs::path dir_;
    std::string consoleCapture_;  // ~"..." output of a Console() command
    bool capturing_ = false;
};
