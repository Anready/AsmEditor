#pragma once

#include "common.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <thread>

// Messages posted to the UI window by the debugger thread.
constexpr UINT WM_DEBUG_STOPPED = WM_APP + 20;  // lp = DebugStop*
constexpr UINT WM_DEBUG_EXITED = WM_APP + 21;   // wp = exit code
constexpr UINT WM_DEBUG_LOG = WM_APP + 22;      // wp = Out, lp = std::string* (UTF-8)

struct SourceLocation {
    fs::path file;
    int line = 0;
    bool valid() const { return line > 0; }
    bool operator==(const SourceLocation& o) const { return line == o.line && SamePath(file, o.file); }
};

struct DebugStop {
    enum class Reason { Breakpoint, Step, Entry, Pause, Exception, Int3 } reason;
    std::wstring message;     // extra details (exception text)
    SourceLocation location;  // empty when there is no source for the address
    CONTEXT context;
};

// Source-level debugger for x64 executables built with NASM (-g -F cv8) and link /debug.
// The Win32 debug loop runs on its own thread; the UI sends commands and reads state while stopped.
class Debugger {
public:
    using Breakpoints = std::map<std::wstring, std::set<int>>;  // Key(path) -> lines

    explicit Debugger(HWND notify);
    ~Debugger();
    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    // Starts the program under the debugger, attached to the given pseudoconsole.
    bool Start(const fs::path& exe, const fs::path& cwd, HPCON console,
               const Breakpoints& breakpoints, bool stopAtEntry, std::wstring& error);

    void Continue() { Send(Command::Continue); }
    void StepInto() { Send(Command::StepInto); }
    void StepOver() { Send(Command::StepOver); }
    void StepOut() { Send(Command::StepOut); }
    void Pause();
    void Stop();
    void SetBreakpoints(const fs::path& file, const std::set<int>& lines);

    bool Running() const { return running_; }
    bool Stopped() const { return stopped_; }

    // Valid only while stopped.
    const CONTEXT& Context() const { return context_; }
    size_t ReadMemory(uint64_t address, void* buffer, size_t size);
    std::optional<uint64_t> Evaluate(const std::string& expression, std::wstring& error);
    std::optional<uint64_t> LookupSymbol(const std::string& name);
    std::wstring SymbolAt(uint64_t address);

    static std::wstring Key(const fs::path& p) { return Lower(NormalizePath(p).wstring()); }
    static std::optional<uint64_t> Register(const CONTEXT& c, const std::string& lowerName);

private:
    enum class Command { None, Continue, StepInto, StepOver, StepOut, Stop };
    enum class StepMode { None, Into, Over, Out };

    struct Breakpoint {
        uint64_t address = 0;
        uint8_t original = 0;
        bool user = false;   // set in the editor
        bool entry = false;  // one-shot stop at main
        bool temp = false;   // internal, for stepping over calls
    };

    void Send(Command c);
    void Loop(fs::path exe, fs::path cwd, HPCON console, Breakpoints bps, bool stopAtEntry,
              std::promise<std::wstring>* started);
    DWORD OnException(const DEBUG_EVENT& ev);
    void OnStep(HANDLE thread, CONTEXT& ctx);
    void CheckStop(HANDLE thread, CONTEXT& ctx);
    void StopAt(HANDLE thread, CONTEXT& ctx, DebugStop::Reason reason, const std::wstring& message);
    void Resume(HANDLE thread, CONTEXT& ctx, bool singleStep);
    void ContinueStepping(HANDLE thread, CONTEXT& ctx);
    void RunFree(HANDLE thread, CONTEXT& ctx);

    bool Install(uint64_t address, bool user, bool entry, bool temp);
    void Remove(uint64_t address);
    void RemoveTemps();
    void InstallFile(const std::wstring& key, const std::set<int>& lines);
    void RemoveUserFile(const std::wstring& key);

    SourceLocation LocationOf(uint64_t address);
    uint64_t ReadQword(uint64_t address);

    HWND notify_;
    std::thread thread_;
    std::recursive_mutex mutex_;  // breakpoints and DbgHelp (which is not thread-safe)
    std::mutex commandMutex_;
    std::condition_variable cv_;
    Command command_ = Command::None;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> pauseRequested_{false};

    std::atomic<HANDLE> process_{nullptr};
    struct Module {
        uint64_t base;
        uint64_t size;
        std::wstring name;
    };

    bool InMainImage(uint64_t a) const { return a >= imageBase_ && a < imageBase_ + imageSize_; }
    uint64_t ImageSize(uint64_t base);

    HANDLE mainThread_ = nullptr;
    std::map<DWORD, HANDLE> threads_;
    uint64_t imageBase_ = 0;
    uint64_t imageSize_ = 0;
    std::map<uint64_t, Module> modules_;  // DLLs by base; DbgHelp only handles the main image (it is slow on DLLs)
    bool loaderBreakpointSeen_ = false;

    std::map<uint64_t, Breakpoint> breakpoints_;
    std::map<std::wstring, std::vector<uint64_t>> fileBreakpoints_;  // Key(path) -> user breakpoint addresses
    std::set<uint64_t> removed_;                                      // removed while the program was running
    uint64_t reinsertAt_ = 0;
    std::vector<uint64_t> lineAddresses_;  // every source line of the program (for Pause)

    StepMode stepMode_ = StepMode::None;
    SourceLocation stepFrom_;
    uint64_t prevRip_ = 0;
    uint64_t prevRsp_ = 0;

    alignas(16) CONTEXT context_{};
};
