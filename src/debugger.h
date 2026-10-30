#pragma once

#include "common.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <thread>

// Messages posted to the UI window by the debugger thread.
constexpr UINT WM_DEBUG_STOPPED = WM_APP + 20;  // lp = DebugStop*
constexpr UINT WM_DEBUG_EXITED = WM_APP + 21;   // wp = exit code, lp = std::string* (run time and memory)
constexpr UINT WM_DEBUG_LOG = WM_APP + 22;      // wp = Out, lp = std::string* (UTF-8)
constexpr UINT WM_DEBUG_ODS = WM_APP + 23;      // lp = std::string* (OutputDebugString text)

struct SourceLocation {
    fs::path file;
    int line = 0;
    bool valid() const { return line > 0; }
    bool operator==(const SourceLocation& o) const { return line == o.line && SamePath(file, o.file); }
};

struct DebugStop {
    enum class Reason { Breakpoint, Step, Entry, Pause, Exception, Int3, Watchpoint, RunTo } reason;
    std::wstring message;     // extra details (exception text, changed variable)
    SourceLocation location;  // empty when there is no source for the address
    CONTEXT context;
};

// A breakpoint on data: debug registers DR0-DR3 stop the program after it writes to the watched bytes.
struct DataWatch {
    std::string expression;  // as typed (for display)
    uint64_t address = 0;
    int size = 8;            // 1, 2, 4 or 8
    uint64_t value = 0;      // value at the last stop
};

struct LaunchOptions {
    fs::path exe;
    std::wstring args;         // command line arguments after the program name
    fs::path cwd;
    std::wstring environment;  // CreateProcess block (empty = inherit)
};

// Source-level debugger for x64 and 32-bit (WOW64) executables built with NASM (-g -F cv8) and link /debug.
// The Win32 debug loop runs on its own thread; the UI sends commands and reads state while stopped.
class Debugger {
public:
    // Key(path) -> line -> condition ("" = always; "rcx == 5", "#10" = on the 10th hit, "#10 rax > 0").
    using Breakpoints = std::map<std::wstring, std::map<int, std::string>>;

    struct Frame {
        uint64_t pc = 0;          // address in the function
        uint64_t returnSlot = 0;  // stack address of the return address that leads back here (0 for frame 0)
        SourceLocation location;
        std::wstring function;    // symbol (+offset) or module+offset
    };

    explicit Debugger(HWND notify);
    ~Debugger();
    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    // Starts the program under the debugger, attached to the given pseudoconsole.
    bool Start(const LaunchOptions& launch, HPCON console, const Breakpoints& breakpoints, bool stopAtEntry,
               std::optional<SourceLocation> runTo, std::wstring& error);

    void Continue() { Send(Command::Continue); }
    void StepInto() { Send(Command::StepInto); }
    void StepOver() { Send(Command::StepOver); }
    void StepOut() { Send(Command::StepOut); }
    bool RunTo(const SourceLocation& where);  // continue to a line (false: no code there)
    void Pause();
    void Stop();
    void SetBreakpoints(const fs::path& file, const std::map<int, std::string>& lines);

    bool Running() const { return running_; }
    bool Stopped() const { return stopped_; }
    bool Is32() const { return wow64_; }

    // Valid only while stopped.
    const CONTEXT& Context() const { return context_; }
    size_t ReadMemory(uint64_t address, void* buffer, size_t size);
    bool WriteMemory(uint64_t address, const void* data, size_t size);
    bool SetRegister(const std::string& lowerName, uint64_t value);  // applied when the program continues
    bool SetNextStatement(const SourceLocation& where, std::wstring& error);
    std::optional<uint64_t> Evaluate(const std::string& expression, std::wstring& error);
    std::optional<uint64_t> LookupSymbol(const std::string& name);
    std::wstring SymbolAt(uint64_t address);
    SourceLocation LocationOf(uint64_t address);
    std::optional<uint64_t> AddressOfLine(const SourceLocation& where);
    std::vector<Frame> CallStack();
    int PointerSize() const { return wow64_ ? 4 : 8; }

    // Data breakpoints (at most 4). Applied to the program's threads when it continues.
    bool AddDataWatch(const DataWatch& watch, std::wstring& error);
    void RemoveDataWatch(size_t index);
    std::vector<DataWatch> DataWatches();

    static std::wstring Key(const fs::path& p) { return Lower(NormalizePath(p).wstring()); }
    static std::optional<uint64_t> Register(const CONTEXT& c, const std::string& lowerName);
    static bool SetRegisterIn(CONTEXT& c, const std::string& lowerName, uint64_t value);

private:
    enum class Command { None, Continue, StepInto, StepOver, StepOut, Stop };
    enum class StepMode { None, Into, Over, Out };

    struct Breakpoint {
        uint64_t address = 0;
        uint8_t original = 0;
        bool user = false;   // set in the editor
        bool entry = false;  // one-shot stop at main
        bool temp = false;   // internal, for stepping over calls
        bool runTo = false;  // one-shot "run to cursor"
        std::string condition;
        int hits = 0;
    };

    void Send(Command c);
    void Loop(LaunchOptions launch, HPCON console, Breakpoints bps, bool stopAtEntry,
              std::optional<SourceLocation> runTo, std::promise<std::wstring>* started);
    DWORD OnException(const DEBUG_EVENT& ev);
    void OnStep(HANDLE thread, CONTEXT& ctx);
    void CheckStop(HANDLE thread, CONTEXT& ctx);
    void StopAt(HANDLE thread, CONTEXT& ctx, DebugStop::Reason reason, const std::wstring& message);
    void Resume(HANDLE thread, CONTEXT& ctx, bool singleStep);
    void ContinueStepping(HANDLE thread, CONTEXT& ctx);
    void RunFree(HANDLE thread, CONTEXT& ctx);
    bool ConditionHolds(Breakpoint& bp, const CONTEXT& ctx);
    std::wstring WatchHit(uint64_t dr6);

    bool Install(uint64_t address, bool user, bool entry, bool temp);
    void Remove(uint64_t address);
    void RemoveTemps();
    void InstallFile(const std::wstring& key, const std::map<int, std::string>& lines);
    void RemoveUserFile(const std::wstring& key);

    bool GetContext(HANDLE thread, CONTEXT& ctx);
    bool SetContext(HANDLE thread, const CONTEXT& ctx, bool full);
    uint64_t ReadPointer(uint64_t address);
    bool LooksLikeReturnAddress(uint64_t value);

    HWND notify_;
    std::thread thread_;
    std::recursive_mutex mutex_;  // breakpoints and DbgHelp (which is not thread-safe)
    std::mutex commandMutex_;
    std::condition_variable cv_;
    Command command_ = Command::None;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> pauseRequested_{false};
    bool contextChanged_ = false;  // registers edited while stopped: write the full context back
    bool foreignStop_ = false;     // stopped on a thread that did not report the event (pause in a system call)
    bool stepFromForeign_ = false; // after such a stop: the next temporary line breakpoint ends the step
    bool watchesChanged_ = false;  // debug registers must be rewritten
    bool wow64_ = false;

    std::atomic<HANDLE> process_{nullptr};
    struct Module {
        uint64_t base;
        uint64_t size;
        std::wstring name;
    };

    bool InMainImage(uint64_t a) const { return a >= imageBase_ && a < imageBase_ + imageSize_; }
    uint64_t ImageSize(uint64_t base);

    HANDLE mainThread_ = nullptr;
    DWORD mainThreadId_ = 0;
    std::map<DWORD, HANDLE> threads_;
    std::set<DWORD> watchThreads_;  // threads whose debug registers are up to date
    uint64_t imageBase_ = 0;
    uint64_t imageSize_ = 0;
    std::map<uint64_t, Module> modules_;  // DLLs by base; DbgHelp only handles the main image (it is slow on DLLs)
    bool loaderBreakpointSeen_ = false;
    bool wowLoaderBreakpointSeen_ = false;
    ULONGLONG startTick_ = 0;

    std::map<uint64_t, Breakpoint> breakpoints_;
    std::map<std::wstring, std::vector<uint64_t>> fileBreakpoints_;  // Key(path) -> user breakpoint addresses
    std::set<uint64_t> removed_;                                      // removed while the program was running
    uint64_t reinsertAt_ = 0;
    std::vector<uint64_t> lineAddresses_;  // every source line of the program (for Pause)
    std::vector<DataWatch> watches_;

    StepMode stepMode_ = StepMode::None;
    SourceLocation stepFrom_;
    uint64_t prevRip_ = 0;
    uint64_t prevRsp_ = 0;

    alignas(16) CONTEXT context_{};
};
