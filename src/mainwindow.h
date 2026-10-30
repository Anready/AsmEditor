#pragma once

#include "common.h"
#include "completion.h"
#include "debugger.h"
#include "editor.h"
#include "terminal.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class MainWindow {
public:
    bool Create(HINSTANCE inst, Settings* settings);
    HWND hwnd() const { return hwnd_; }
    bool PreTranslate(MSG& msg);
    void OpenPath(const fs::path& path);
    void RestoreSession();

private:
    struct ToolItem {
        int id;
        std::wstring text;
        int icon;      // see DrawIcon
        bool menu;
        bool right;
        int show = 0;  // 0 always, 1 only while debugging, 2 only when not debugging
        RECT rc{};
    };
    enum class After { Nothing, Run, Debug, DebugEntry };
    using Runs = std::vector<std::pair<std::string, int>>;  // text + panel style
    struct CheckJob {
        int editorId;
        int version;
        std::string text;
        std::vector<fs::path> includes;
        Settings settings;
    };
    enum class Drag { None, Tree, Console, Debug };

    // window plumbing
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK ConsoleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void CreateChildren();
    void CreateFonts();
    void BuildMenus();
    void Layout();
    void Paint(HDC dc);
    void PaintToolbar(HDC dc);
    void PaintTabs(HDC dc);
    void PaintStatus(HDC dc);
    void PaintWelcome(HDC dc);
    void DrawIcon(HDC dc, int icon, const RECT& rc, bool enabled);
    void OnMouseMove(int x, int y);
    void OnClick(int x, int y, bool middle);
    void OnCommand(int id);
    void OnNotify(NMHDR* nm);
    void UpdateTitle();
    void InvalidateChrome();
    int S(int v) const { return Scale(v, dpi_); }

    // editors
    Editor* Active() const { return active_ >= 0 && active_ < (int)editors_.size() ? editors_[active_].get() : nullptr; }
    Editor* FindByHwnd(HWND h) const;
    Editor* FindById(int id) const;
    int FindByPath(const fs::path& p) const;
    void AddEditor(std::unique_ptr<Editor> ed);
    void Activate(int index);
    bool ConfirmClose(int index);
    void CloseEditor(int index);
    bool CloseAll();
    Editor* OpenFile(const fs::path& path, int line = 0);
    bool Save(Editor* ed);
    bool SaveAs(Editor* ed);
    void SaveAll();
    void ApplyFontSize(int size);

    // project
    bool OpenProject(const fs::path& dir);
    void CloseProject();
    void SaveProjectSession();
    void NewProject();
    void SetMainFile(const fs::path& f);
    void RefreshTree();
    void BuildTree(HTREEITEM parent, const fs::path& dir, int depth);
    fs::path TreePath(HTREEITEM item) const;
    void TreeContextMenu();
    void AddRecent(const fs::path& dir);
    std::optional<fs::path> PickFolder(const wchar_t* title);

    // build & run
    std::vector<fs::path> IncludeDirs(const fs::path& file) const;
    fs::path BuildTarget();
    void StartBuild(After after, bool rebuild = false);
    void OnBuildDone(BuildResult* r, After after, const fs::path& src);
    void RunProgram(const fs::path& exe, const fs::path& cwd, After mode);
    void RunExternal(const fs::path& exe, const fs::path& cwd);
    void StopProgram();
    void OnProgramExit(DWORD code, DWORD generation);
    // program console (ConPTY)
    bool StartConsole();
    void EndConsole();
    void FinishConsole(DWORD exitCode);
    void OnConsoleDone(DWORD exitCode);
    void RenderTerminal();
    void SendToProgram(const std::string& bytes);
    bool ConsoleKey(UINT msg, WPARAM wp);
    std::pair<int, int> ConsoleGridSize() const;
    void TrimConsole();
    void Print(const std::string& utf8, Out style);
    void PostOutput(const std::string& utf8, Out style);
    void ConsoleDoubleClick(Sci_Position pos);
    void RequestCheck(Editor* ed);
    void CheckWorker();
    void RefreshToolStatus();
    UINT ProgramCodePage() const;

    // debugging
    bool DebugStopped() const { return debugger_ && debugger_->Stopped(); }
    void DebugCommand(int id);
    void OnDebugStopped(std::unique_ptr<DebugStop> stop);
    void OnDebugExited(DWORD code);
    void SetDebugging(bool on);
    void ClearExecutionLine();
    void ToggleBreakpoint(Editor* ed, int line);
    void StashBreakpoints(Editor* ed);
    Debugger::Breakpoints CollectBreakpoints() const;
    void RenderPanel(Editor& view, const Runs& runs);
    void RenderRegisters();
    void RenderMemory();
    std::string DebugTipText(Editor* ed, Sci_Position pos);
    void ShowHint(Editor* ed, Sci_Position pos);
    void ShowSignature(Editor* ed);
    void SetupPanelView(Editor& view);
    bool DebugPanelVisible() const { return debugging_ || showDebugPanel_; }

    // completion
    void Complete(Editor* ed, bool explicitRequest);
    std::vector<Completion::Symbol> OtherFileSymbols(const Editor* current);

    // find
    void ShowFind(bool replace);
    void OnFindMessage(FINDREPLACEW* fr);
    bool FindNext(bool forward);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tree_ = nullptr;
    std::unique_ptr<Editor> console_;
    HACCEL accel_ = nullptr;
    HMENU fileMenu_ = nullptr, editMenu_ = nullptr, viewMenu_ = nullptr, runMenu_ = nullptr, recentMenu_ = nullptr;
    HFONT uiFont_ = nullptr, uiBold_ = nullptr, titleFont_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    int dpi_ = 96;
    Settings* settings_ = nullptr;

    std::vector<std::unique_ptr<Editor>> editors_;
    int active_ = -1;
    std::vector<ToolItem> tools_;
    std::vector<RECT> tabRects_, closeRects_;
    std::vector<std::pair<RECT, std::wstring>> welcomeLinks_;
    int tabScroll_ = 0;
    bool ensureTabVisible_ = false;
    int hoverTool_ = -1, hoverTab_ = -1, hoverClose_ = -1, hoverLink_ = -1;
    bool hoverClear_ = false, hoverRefresh_ = false;
    Drag drag_ = Drag::None;

    RECT rcToolbar_{}, rcStatus_{}, rcTreeHeader_{}, rcTabs_{}, rcEditor_{}, rcConsoleHeader_{};
    RECT rcSplitV_{}, rcSplitH_{}, rcClear_{}, rcRefresh_{};
    RECT rcDebug_{}, rcSplitD_{}, rcDebugHeader_{}, rcMemHeader_{}, rcMemRow_{};

    std::unique_ptr<Debugger> debugger_;
    bool debugging_ = false;
    bool showDebugPanel_ = false;
    std::unique_ptr<Editor> regsView_, memView_;
    HWND memExpr_ = nullptr;
    std::wstring debugStatus_;
    COLORREF debugStatusColor_ = Theme::Muted;
    fs::path execFile_;
    alignas(16) CONTEXT lastContext_{};
    bool haveLastContext_ = false;
    std::map<std::wstring, std::set<int>> breakpoints_;  // Debugger::Key(path) -> lines, for files not open
    std::set<std::wstring> excludedFromBuild_;  // Project::Key of files the user excluded from the build
    struct CachedSymbols {
        fs::file_time_type time;
        std::vector<Completion::Symbol> symbols;
        fs::path path;
    };
    std::map<std::wstring, CachedSymbols> symbolCache_;  // project files that are not open

    // go to definition
    struct SymbolHit {
        Editor* editor;  // open file (or nullptr)
        fs::path file;
        Completion::Symbol symbol;
    };
    struct NavLocation {
        fs::path file;
        int editorId;
        Sci_Position pos;
    };
    std::vector<SymbolHit> ProjectSymbols(const Editor* current);
    void GoToDefinition(Editor* ed, Sci_Position pos);
    bool OpenIncludeAt(Editor* ed, Sci_Position pos);
    void JumpTo(const SymbolHit& hit);
    NavLocation CurrentLocation();
    void PushNavigation();
    void Navigate(bool back);
    void UpdateLinkHover(Editor* ed, bool ctrl);
    void ClearLinkHover();
    struct UsageSource {
        fs::path file;
        Editor* editor;  // open tab, or nullptr for a file read from disk
        std::string text;
    };
    bool SymbolUnderCaret(Editor* ed, Sci_Position pos, std::string& word, std::string& scope);
    std::vector<UsageSource> UsageSources(Editor* ed, bool local);
    void FindUsages(Editor* ed, Sci_Position pos);
    void Rename(Editor* ed, Sci_Position pos);
    void ClearUsageHighlight();
    static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    std::vector<NavLocation> navBack_, navForward_;
    Editor* linkEditor_ = nullptr;  // editor showing the Ctrl+hover underline
    Sci_Position linkStart_ = 0, linkEnd_ = 0;
    Editor* usageEditor_ = nullptr;  // editor with highlighted usages

    fs::path projectRoot_;
    fs::path mainFile_;
    std::vector<fs::path> treePaths_;
    HTREEITEM contextItem_ = nullptr;

    bool building_ = false;
    HANDLE process_ = nullptr, job_ = nullptr;
    DWORD generation_ = 0;

    HPCON pty_ = nullptr;           // pseudoconsole of the running program
    HANDLE ptyInput_ = nullptr;     // keyboard input to the program
    std::thread ptyReader_;
    Terminal term_;
    bool termActive_ = false;       // the end of the console view mirrors the terminal screen
    Sci_Position termStart_ = 0;    // where the terminal screen starts in the console view
    DWORD consoleSession_ = 0;
    bool renderPending_ = false;
    wchar_t pendingSurrogate_ = 0;
    std::wstring toolStatus_;
    bool toolStatusWarn_ = false;

    std::thread checkThread_;
    std::mutex checkMutex_;
    std::condition_variable checkCv_;
    std::deque<CheckJob> checkQueue_;
    bool checkStop_ = false;

    HWND findDialog_ = nullptr;
    UINT findMsg_ = 0;
    FINDREPLACEW findData_{};
    wchar_t findText_[256] = L"";
    wchar_t replaceText_[256] = L"";
    std::wstring lastFind_;
    DWORD lastFindFlags_ = FR_DOWN;
};
