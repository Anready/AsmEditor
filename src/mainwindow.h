#pragma once

#include "common.h"
#include "completion.h"
#include "debugger.h"
#include "editor.h"
#include "terminal.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

class UiTest;

class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    bool Create(HINSTANCE inst, Settings* settings);
    HWND hwnd() const { return hwnd_; }
    bool PreTranslate(MSG& msg);
    void OpenPath(const fs::path& path);
    void RestoreSession();
    void StartUiTest(const fs::path& out);  // --ui-test: see uitest.cpp

private:
    friend class UiTest;
    void UiTestTick();
    std::unique_ptr<UiTest> uiTest_;
    int uiTestExit_ = 0;
    bool compactTools_ = false;  // narrow window: toolbar buttons show icons only

    struct ToolItem {
        int id;
        const wchar_t* text;  // Russian; shown through Tr
        int icon;      // see DrawIcon
        bool menu;
        bool right;
        int show = 0;  // 0 always, 1 only while debugging, 2 only when not debugging
        RECT rc{};
    };
    enum class After { Nothing, Run, Debug, DebugEntry, RunToCursor, Profile, Sample };
    using Runs = std::vector<std::pair<std::string, int>>;  // text + panel style
    struct CheckJob {
        int editorId;
        int version;
        std::string text;
        std::vector<fs::path> includes;
        Settings settings;
        RunConfig config;
    };
    enum class Drag { None, Tree, Console, Debug, Tab, Split };
    // Tabs of the bottom panel. The terminal shows only the program's console; messages go to the others.
    enum class Pane { Output, Terminal, Problems, Search, DebugOutput, Count };
    enum class LeftTab { Project, Outline };
    enum class DebugTab { Registers, Stack, Watch, Disasm, Flags, Count };
    enum class MemFormat { Bytes, Words, Dwords, Qwords, Float, Double, Text, Count };

    // window plumbing
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK ConsoleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK MarkBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
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
    void PaintHeaderTabs(HDC dc, const RECT& area, const std::vector<std::wstring>& titles, int active,
                         std::vector<RECT>& rects, int hover);
    void DrawIcon(HDC dc, int icon, const RECT& rc, bool enabled);
    std::wstring ToolLabel(const ToolItem& t) const;  // text of a toolbar item (drop-downs show their value)
    void OnMouseMove(int x, int y);
    void OnClick(int x, int y, bool middle);
    void OnRightClick(int x, int y);
    void OnCommand(int id);
    void OnNotify(NMHDR* nm);
    void UpdateTitle();
    void InvalidateChrome();
    int S(int v) const { return Scale(v, dpi_); }

    // commands: names, shortcuts (customizable), menus and the command palette
    struct CommandDef {
        int id;
        const wchar_t* name;  // Russian; shown through Tr
        const wchar_t* keys;  // default shortcut, "" for none
    };
    static const std::vector<CommandDef>& Commands();
    std::wstring KeysFor(int id) const;
    std::wstring MenuText(int id) const;
    void AppendCommand(HMENU menu, int id);
    void BuildAccelerators();
    void CommandPalette();
    void QuickOpen();
    void EditKeys();
    void ToggleAssociation();  // Help → open .asm files in AsmEditor (HKCU)

    // editors
    Editor* Active() const { return active_ >= 0 && active_ < (int)editors_.size() ? editors_[active_].get() : nullptr; }
    Editor* FindByHwnd(HWND h) const;
    Editor* FindById(int id) const;
    int FindByPath(const fs::path& p) const;
    int IndexOf(const Editor* ed) const;
    void AddEditor(std::unique_ptr<Editor> ed);
    void Activate(int index);
    bool ConfirmClose(int index);
    void CloseEditor(int index);
    bool CloseAll();
    void CloseOthers(int keep);
    Editor* OpenFile(const fs::path& path, int line = 0);
    bool Save(Editor* ed);
    bool SaveAs(Editor* ed);
    void SaveAll();
    void ApplyFontSize(int size);
    void RestyleAll();
    void ApplyTheme(const std::wstring& id);
    void Relocalize();
    void TabMenu(int index);
    void TogglePin(int index);
    void MoveTab(int from, int to);
    void SortPinnedTabs();
    void SplitWith(int index);
    void Unsplit();
    Editor* SplitEditor() const { return splitIndex_ >= 0 && splitIndex_ < (int)editors_.size() ? editors_[splitIndex_].get() : nullptr; }
    std::vector<Editor*> PanelViews() const;

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
    std::vector<fs::path> ProgramFiles();  // project files with their own main
    std::vector<fs::path> ProjectFiles();  // every file of the project tree
    void ShowLeftTab(LeftTab tab);
    void RefreshOutline();
    // external changes, autosave and crash recovery
    void CheckExternalChanges();
    fs::path BackupDir() const;
    void WriteBackups();
    void RemoveBackup(const Editor* ed);
    void OfferRecovery();

    // build & run
    std::vector<fs::path> IncludeDirs(const fs::path& file) const;
    fs::path BuildTarget();
    void StartBuild(After after, bool rebuild = false);
    void OnBuildDone(BuildResult* r, After after, const fs::path& src);
    void RunProgram(const fs::path& exe, const fs::path& cwd, After mode);
    void RunExternal(const fs::path& exe, const fs::path& cwd);
    void RunInWsl(const fs::path& exe, const fs::path& cwd, bool debug);
    void StopProgram();
    void OnProgramExit(DWORD code, DWORD generation);
    fs::path RunDirectory(const fs::path& src) const;
    LaunchOptions MakeLaunch(const fs::path& exe, const fs::path& cwd) const;
    void FeedStdin();
    void EditRunConfig();
    void ChooseTarget();
    void ChooseProfile();
    void CheckSolution();
    void NewExercise();
    void OpenLesson();
    void ExerciseHint();
    void GroupSummary();
    std::map<std::string, size_t> hintsShown_;  // exercise -> hints given
    void ExportResults();
    void RecordResult(const std::string& id, int passed, int total);
    static fs::path ResultsFile();
    void CheckUpdates();
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
    void Print(const std::string& utf8, Out style);  // to the Output tab
    void PrintTo(Pane pane, const std::string& utf8, Out style);
    void ClearPane(Pane pane);
    void ShowPane(Pane pane);
    Editor& View(Pane pane) const { return *views_[static_cast<int>(pane)]; }
    void SetupOutputView(Editor& view);
    void RefreshProblems();
    void ConsoleDoubleClick(Editor& view, Sci_Position pos);
    void RequestCheck(Editor* ed);
    void CheckWorker();
    void RefreshToolStatus();
    UINT ProgramCodePage() const;

    // debugging
    bool DebugStopped() const { return debugger_ && debugger_->Stopped(); }
    void DebugCommand(int id);
    void OnDebugStopped(std::unique_ptr<DebugStop> stop);
    void OnDebugExited(DWORD code, const std::string& stats);
    void SetDebugging(bool on);
    void ClearExecutionLine();
    void ToggleBreakpoint(Editor* ed, int line);
    void EditBreakpointCondition(Editor* ed, int line, bool logpoint = false);
    void BreakpointMenu(Editor* ed, int line);
    void StashBreakpoints(Editor* ed);
    Debugger::Breakpoints CollectBreakpoints() const;
    void RunToCursor();
    void SetNextStatement();
    void RenderPanel(Editor& view, const Runs& runs);
    void RenderDebugPanel();  // the visible tab and the memory view
    void RenderRegisters();
    void RenderCallStack();
    void RenderWatches();
    void RenderDisassembly();
    void RenderMemory();
    void RenderFlags();
    void StepBack();
    void OnProfile(std::vector<ProfileLine> lines);
    void ShowProfileNotes(Editor* ed);
    void ClearProfile();
    void ShowDebugTab(DebugTab tab);
    void PanelDoubleClick(Editor& view, Sci_Position pos);
    void EditRegister(const std::string& name);
    void EditMemory(uint64_t address);
    void AddWatch(const std::string& expression);
    void AddDataBreakpoint(const std::string& expression);
    std::string DebugTipText(Editor* ed, Sci_Position pos);
    void ShowHint(Editor* ed, Sci_Position pos);
    std::string SymbolHint(Editor* ed, const std::string& word);
    void ShowSignature(Editor* ed);
    void SetupPanelView(Editor& view);
    bool DebugPanelVisible() const { return debugging_ || showDebugPanel_; }

    // completion, snippets, formatting, multiple carets
    void Complete(Editor* ed, bool explicitRequest);
    std::vector<Completion::Symbol> OtherFileSymbols(const Editor* current);
    void ShowSnippets(Editor* ed);
    void InsertSnippet(Editor* ed, const std::string& name);
    void FormatCode(Editor* ed);
    void AddCaret(Editor* ed, bool down);
    void HighlightOccurrences(Editor* ed);
    void UpdateBracePairs(Editor* ed);
    void FoldAll(Editor* ed, bool fold);
    void QuickFix(Editor* ed);                           // Alt+Enter: fixes for the problems on the caret line
    void ShowCallArguments(Editor* ed);                  // while typing "call name": its arguments
    std::string CallArgumentsText(Editor* ed, int line); // which argument registers are set before a call

    // find
    void ShowFind(bool replace);
    void OnFindMessage(FINDREPLACEW* fr);
    bool FindNext(bool forward);
    void FindInFiles();

    // mark bar: errors, warnings, breakpoints and search hits along the vertical scrollbar
    void PaintMarkBar(HWND bar, HDC dc);
    void MarkBarClick(HWND bar, int y);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tree_ = nullptr;
    HWND outline_ = nullptr;
    HWND markBar_ = nullptr, markBar2_ = nullptr;
    std::unique_ptr<Editor> views_[static_cast<int>(Pane::Count)];  // bottom panel
    Pane pane_ = Pane::Output;
    std::vector<RECT> paneRects_, leftTabRects_, debugTabRects_;
    int hoverPane_ = -1, hoverLeftTab_ = -1, hoverDebugTab_ = -1;
    LeftTab leftTab_ = LeftTab::Project;
    std::vector<int> outlineLines_;  // outline item lParam -> line
    int outlineEditor_ = 0;          // editor id the outline shows
    HACCEL accel_ = nullptr;
    HMENU fileMenu_ = nullptr, editMenu_ = nullptr, viewMenu_ = nullptr, runMenu_ = nullptr, recentMenu_ = nullptr;
    HMENU helpMenu_ = nullptr, themeMenu_ = nullptr;
    HFONT uiFont_ = nullptr, uiBold_ = nullptr, titleFont_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    int dpi_ = 96;
    Settings* settings_ = nullptr;

    std::vector<std::unique_ptr<Editor>> editors_;
    int active_ = -1;
    int splitIndex_ = -1;     // editor shown in the right half (-1: not split)
    bool focusRight_ = false; // the right half has the focus: tabs open there
    std::vector<ToolItem> tools_;
    std::vector<RECT> tabRects_, closeRects_;
    std::vector<std::pair<RECT, std::wstring>> welcomeLinks_;
    int tabScroll_ = 0;
    bool ensureTabVisible_ = false;
    int hoverTool_ = -1, hoverTab_ = -1, hoverClose_ = -1, hoverLink_ = -1;
    bool hoverClear_ = false, hoverRefresh_ = false, hoverMemFormat_ = false;
    Drag drag_ = Drag::None;
    int dragTab_ = -1, dragStartX_ = 0;
    bool dragMoved_ = false;

    RECT rcToolbar_{}, rcStatus_{}, rcTreeHeader_{}, rcTabs_{}, rcEditor_{}, rcEditor2_{}, rcConsoleHeader_{};
    RECT rcSplitV_{}, rcSplitH_{}, rcSplitE_{}, rcClear_{}, rcRefresh_{};
    RECT rcDebug_{}, rcSplitD_{}, rcDebugHeader_{}, rcMemHeader_{}, rcMemRow_{}, rcWatchRow_{}, rcMemFormat_{};

    std::unique_ptr<Debugger> debugger_;
    bool debugging_ = false;
    bool showDebugPanel_ = false;
    DebugTab debugTab_ = DebugTab::Registers;
    std::unique_ptr<Editor> regsView_, memView_, stackView_, watchView_, disasmView_, flagsView_;
    HWND memExpr_ = nullptr, watchExpr_ = nullptr;
    MemFormat memFormat_ = MemFormat::Bytes;
    std::vector<uint8_t> prevMem_;    // bytes shown at the previous stop, to highlight changes
    uint64_t prevMemAddress_ = 0;
    std::vector<std::string> watches_;  // expressions of the watch list
    std::vector<std::string> regLines_;      // register name per line of the registers view
    std::vector<uint64_t> memLines_;         // address per line of the memory view
    std::vector<int> stackLines_;            // frame per line of the call stack view (-1: none)
    std::vector<int> watchLines_;            // watch (>= 0) or data breakpoint (-2 - index) per line
    std::vector<int> flagLines_;             // flag bit per line of the flags view (-1: none)
    std::vector<uint64_t> disasmLines_;      // address per line of the disassembly
    std::vector<Debugger::Frame> frames_;
    std::wstring debugStatus_;
    COLORREF debugStatusColor_ = 0;
    fs::path execFile_;
    alignas(16) CONTEXT lastContext_{};
    bool haveLastContext_ = false;
    std::vector<ProfileLine> profile_;          // last profiler run (notes in the editors)
    std::vector<uint8_t> prevStack_;            // stack bytes at the previous stop (for the stack animation)
    uint64_t prevStackRsp_ = 0;
    int stackFlash_ = 0;                        // frames left of the fading highlight of changed stack slots
    std::optional<SourceLocation> runTo_;    // "run to cursor" requested before the program was started
    Debugger::Breakpoints breakpoints_;  // Debugger::Key(path) -> lines, for files not open
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
    bool SymbolUnderCaret(Editor* ed, Sci_Position pos, std::string& word, std::string& scope, bool quiet = false);
    std::vector<UsageSource> UsageSources(Editor* ed, bool local);
    void FindUsages(Editor* ed, Sci_Position pos);
    void Rename(Editor* ed, Sci_Position pos);
    void ClearUsageHighlight();
    static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    std::vector<NavLocation> navBack_, navForward_;
    Editor* linkEditor_ = nullptr;  // editor showing the Ctrl+hover underline
    Sci_Position linkStart_ = 0, linkEnd_ = 0;
    Editor* usageEditor_ = nullptr;  // editor with highlighted usages
    std::string occurrenceWord_;     // word whose occurrences are highlighted
    int occurrenceEditor_ = 0;

    fs::path projectRoot_;
    fs::path mainFile_;
    RunConfig runConfig_;
    std::vector<fs::path> treePaths_;
    HTREEITEM contextItem_ = nullptr;

    bool building_ = false;
    HANDLE process_ = nullptr, job_ = nullptr;
    DWORD generation_ = 0;
    ULONGLONG runStartTick_ = 0;
    std::string exitStats_;
    std::vector<Diagnostic> buildDiagnostics_;  // of the last build, for the Problems tab
    int problemCount_ = 0;                      // distinct problems listed there
    struct SolutionCheck {
        std::string exercise;
        fs::path exe;
    };
    std::optional<SolutionCheck> pendingCheck_;

    HPCON pty_ = nullptr;           // pseudoconsole of the running program
    HANDLE ptyInput_ = nullptr;     // keyboard input to the program
    std::thread ptyReader_;
    Terminal term_;
    bool termActive_ = false;       // the end of the terminal view mirrors the terminal screen
    Sci_Position termStart_ = 0;    // where the terminal screen starts in the terminal view
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
    FindInFilesRequest lastFindInFiles_;
};
