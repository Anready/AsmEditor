#pragma once

#include "common.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

// Messages posted to the UI window by the debugger thread.
constexpr UINT WM_DEBUG_STOPPED = WM_APP + 20;  // lp = DebugStop*
constexpr UINT WM_DEBUG_EXITED = WM_APP + 21;   // wp = exit code, lp = std::string* (run time and memory)
constexpr UINT WM_DEBUG_LOG = WM_APP + 22;      // wp = Out, lp = std::string* (UTF-8)
constexpr UINT WM_DEBUG_ODS = WM_APP + 23;      // lp = std::string* (OutputDebugString text)
constexpr UINT WM_DEBUG_PROFILE = WM_APP + 24;  // lp = std::vector<ProfileLine>* (before WM_DEBUG_EXITED)

struct SourceLocation {
    fs::path file;
    int line = 0;
    bool valid() const { return line > 0; }
    bool operator==(const SourceLocation& o) const { return line == o.line && SamePath(file, o.file); }
};

struct DebugStop {
    enum class Reason { Breakpoint, Step, Entry, Pause, Exception, Int3, Watchpoint, RunTo, StepBack } reason;
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
    bool profile = false;      // trace every instruction of the program's own code and count lines
};

// Profiler result for one source line.
struct ProfileLine {
    SourceLocation location;
    uint64_t hits = 0;          // times the line was entered
    uint64_t instructions = 0;  // instructions executed on it
    double seconds = 0;         // time on it, including the functions it calls outside the program (printf...)
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
    // Back to the previous stop: registers, the program's global data and the stack are restored (the heap, files
    // and the console are not). Posts WM_DEBUG_STOPPED. False with a reason when there is nothing to go back to.
    bool StepBack(std::wstring& error);
    size_t HistorySize();
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
    std::string FormatLogMessage(const std::string& text);  // "{expr}" -> value, with the current context
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
    void FillDebugRegisters(CONTEXT& ctx);
    void ApplyWatchesToOtherThreads(HANDLE current, const CONTEXT& ctx);
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

    struct RuntimeFunction {  // IMAGE_RUNTIME_FUNCTION_ENTRY
        DWORD begin, end, unwind;
    };
    uint64_t ModuleBase(uint64_t address);  // 0: not in a loaded module
    const std::vector<RuntimeFunction>& FunctionTable(uint64_t base);
    bool UnwindFrame(uint64_t regs[16], uint64_t& rip, bool caller);
    std::map<uint64_t, std::vector<RuntimeFunction>> functionTables_;

    // Step back: a snapshot per stop.
    struct Snapshot {
        HANDLE thread = nullptr;
        alignas(16) CONTEXT context;
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> memory;
        size_t Bytes() const;
    };
    void RecordSnapshot(HANDLE thread, const CONTEXT& ctx);
    void FindWritableSections();
    std::deque<Snapshot> history_;
    std::vector<std::pair<uint64_t, uint64_t>> writable_;  // address, size of the image's writable sections
    HANDLE stopThread_ = nullptr;

    // Profiler.
    void ProfileStep(const CONTEXT& ctx);
    bool profiling_ = false;
    bool profileLimitHit_ = false;
    uint64_t profileTotal_ = 0;
    int profilePrev_ = -1;
    int64_t profileTick_ = 0;
    std::unordered_map<uint64_t, int> profileIndex_;  // address -> index in profile_ (-1: no source)
    std::map<std::pair<std::wstring, int>, int> profileByLine_;
    std::vector<ProfileLine> profile_;

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
